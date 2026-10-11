#pragma once

#include <cstdint>
#include <optional>

#include "rfeed/book_builder.hpp"
#include "rfeed/line_arbiter.hpp"
#include "rfeed/normalized.hpp"
#include "rfeed/packet.hpp"
#include "rfeed/packet_buffer.hpp"
#include "rfeed/recovery.hpp"
#include "rfeed/transport.hpp"
#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed {

struct HandlerStats {
  std::uint64_t datagrams{0};
  std::uint64_t packet_rejects{0};
  std::uint64_t heartbeats{0};
  std::uint64_t framing_errors{0};
  std::uint64_t message_errors{0};
  std::uint64_t buffered{0};
  std::uint64_t replayed{0};
  std::uint64_t gaps{0};
  std::uint64_t snapshot_cycles{0};
  std::uint64_t resets{0};
  std::uint64_t stale_periods{0};
};

struct HandlerConfig {
  std::size_t book_capacity{4096};
  std::size_t buffer_packets{1024};

  // How long both lines may be silent before the books stop claiming to be
  // current. A feed that simply stops is the failure mode that most resembles a
  // healthy one: no gap is ever declared, because a gap needs a later sequence
  // number to reveal it, and none arrives. Without this the handler sits in
  // Synchronized serving a book frozen at the moment the venue went away.
  // Zero disables the check.
  Timestamp stale_after_ns{2'000'000'000};
};

// One channel, end to end: transport → arbitration → framing → decode → book →
// normalized events, with the recovery FSM gating what reaches the sink.
//
// Templated on the transport so the same code runs against a capture and a
// socket; templated on the venue so there are no venue conditionals inside;
// templated on the sink so the publish call devirtualizes.
template <VenuePolicy V, class TransportT, EventSink Sink>
  requires Transport<TransportT>
class ChannelHandler {
 public:
  using Builder = BookBuilder<V, Sink>;

  ChannelHandler(ChannelId channel, TransportT& transport, Sink& sink,
                 RecoveryConfig recovery = {}, HandlerConfig config = {})
      : transport_{&transport},
        sink_{&sink},
        builder_{channel, sink, config.book_capacity},
        fsm_{recovery},
        buffer_{config.buffer_packets},
        config_{config} {}

  // Starts against a caller-supplied clock. Only correct when that clock is the
  // same one the transport stamps packets with.
  void start(Timestamp now) noexcept {
    deferred_start_ = false;
    apply(fsm_.start(now), now);
  }

  // Starts against the first packet's timestamp instead.
  //
  // This is the right default. Every deadline in the recovery FSM is measured
  // against packet rx_ts, so seeding it from an unrelated clock — a replay whose
  // capture is from last Tuesday, or a live feed reading NIC hardware timestamps
  // that are not steered to wall-clock — makes every cycle deadline expire
  // before the first packet arrives, and the feed reports Failed having never
  // actually failed at anything.
  void start() noexcept { deferred_start_ = true; }

  // Processes one datagram. Returns false once the transport is exhausted.
  bool step() noexcept {
    const auto raw = transport_->poll();
    if (!raw) return false;

    ++stats_.datagrams;
    now_ = raw->rx_ts;
    if (deferred_start_) start(now_);

    switch (raw->kind) {
      case ChannelKind::Incremental:          on_incremental_datagram(*raw); break;
      case ChannelKind::Snapshot:             on_snapshot_datagram(*raw); break;
      case ChannelKind::InstrumentDefinition: on_definition_datagram(*raw); break;
      case ChannelKind::HistoricalReplay:     on_replay_datagram(*raw); break;
    }

    check_gap(now_);
    on_clock(now_);
    return true;
  }

  // Drives the time-based checks without a packet. A live caller must call this
  // when poll() returns nothing, because every deadline here — gap maturity,
  // cycle timeouts, staleness — is measured against a clock that otherwise only
  // advances when data arrives, and the failure being detected is data not
  // arriving.
  void on_clock(Timestamp now) noexcept {
    now_ = now;
    check_staleness(now);
    apply(fsm_.on_tick(now), now);
  }

  void run() noexcept {
    while (step()) {
    }
  }

  [[nodiscard]] FeedState state() const noexcept { return fsm_.state(); }
  [[nodiscard]] bool degraded() const noexcept { return fsm_.degraded(); }
  [[nodiscard]] bool stale() const noexcept { return stale_; }
  [[nodiscard]] Builder& builder() noexcept { return builder_; }
  [[nodiscard]] const Builder& builder() const noexcept { return builder_; }
  [[nodiscard]] const HandlerStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const PacketBuffer& buffer() const noexcept { return buffer_; }
  [[nodiscard]] RecoveryFsm<V>& fsm() noexcept { return fsm_; }

 private:
  using Arbiter = LineArbiter<V>;

  // ─── Incremental channel ───────────────────────────────────────────

  void on_incremental_datagram(const RawPacket& raw) noexcept {
    const auto pkt = PacketView::decode(raw.bytes);
    if (!pkt) {
      ++stats_.packet_rejects;
      return;
    }

    incremental_.accept(pkt->header.seq, raw.bytes, raw.rx_ts, raw.line);

    while (const auto ordered = incremental_.next()) {
      if (buffering_) {
        buffer_.push(ordered->seq, ordered->bytes, ordered->rx_ts);
        ++stats_.buffered;
        // Natural refresh runs in parallel with real recovery: apply the packet
        // too, so there is a usable — explicitly Degraded — book within
        // milliseconds instead of waiting out the snapshot cycle.
        if (fsm_.degraded()) process_packet(ordered->seq, ordered->bytes, ordered->rx_ts);
      } else {
        process_packet(ordered->seq, ordered->bytes, ordered->rx_ts);
      }
    }
  }

  void check_gap(Timestamp now) noexcept {
    if (!incremental_.gap_matured(now)) return;

    const auto gap = incremental_.pending_gap();
    ++stats_.gaps;

    if (incremental_.hard_gap()) {
      apply(fsm_.on_window_blown(now), now);
      incremental_.reprime();  // nothing meaningful to resync to
      return;
    }

    apply(fsm_.on_gap(gap, now), now);
    // Step over the hole so the arbiter keeps delivering; recovery is what fills
    // the missing state, not the arbiter.
    incremental_.resync_to(gap.end + 1);
  }

  // Both lines quiet for longer than the venue's cadence allows. This is an
  // alert, never a book-correctness event: nothing is known to be wrong, we just
  // can no longer claim the book is current. Marking it Stale is the whole
  // point — a frozen book that still reports Live is worse than no book.
  void check_staleness(Timestamp now) noexcept {
    if (config_.stale_after_ns <= 0) return;

    // A feed that has never spoken is not stale, it has not started — and
    // FeedState::AwaitingDefinitions already says that, with a timeout of its
    // own. Reporting startup as staleness would fire an alert on every launch.
    const auto& received = incremental_.stats().received;
    if (received[0] + received[1] == 0) return;

    const bool silent = incremental_.line_silent(LineId::A, now, config_.stale_after_ns) &&
                        incremental_.line_silent(LineId::B, now, config_.stale_after_ns);
    if (silent == stale_) return;
    stale_ = silent;
    if (silent) ++stats_.stale_periods;

    // Only Live↔Stale moves. Recovering, Degraded and Halted all already say
    // something more specific and more urgent than "we have not heard anything".
    const auto from = silent ? BookState::Live : BookState::Stale;
    const auto to   = silent ? BookState::Stale : BookState::Live;
    builder_.books().for_each([from, to](SecurityId, InstrumentBook& book) {
      if (book.state == from) book.state = to;
    });
  }

  // ─── Snapshot channel ──────────────────────────────────────────────

  void on_snapshot_datagram(const RawPacket& raw) noexcept {
    // Only useful while joining. Outside SnapshotSync the loop is ignored, which
    // is also what makes --validate-against-snapshot a separate opt-in mode
    // rather than something the recovery path pays for.
    if (fsm_.state() != FeedState::SnapshotSync) return;

    const auto pkt = PacketView::decode(raw.bytes);
    if (!pkt) {
      ++stats_.packet_rejects;
      return;
    }
    snapshot_line_.accept(pkt->header.seq, raw.bytes, raw.rx_ts, raw.line);

    while (const auto ordered = snapshot_line_.next()) {
      for_each_message(ordered->bytes, [&](const MessageView& msg) {
        const auto outcome = builder_.on_message(msg, ordered->seq, ordered->rx_ts);
        if (!outcome) {
          ++stats_.message_errors;
          return;
        }
        if (!outcome->snapshot) return;

        // The snapshot is installed; the instrument is not authoritative until
        // the buffered incrementals past it have been replayed.
        builder_.set_state(outcome->snapshot->security, BookState::Recovering);

        if (fsm_.snapshot_cycle().observe(*outcome->snapshot)) {
          complete_snapshot_cycle(ordered->rx_ts);
        }
      });
    }
  }

  void complete_snapshot_cycle(Timestamp now) noexcept {
    // Replay the whole buffer once. RecoveryReplay mode applies an entry only to
    // instruments still Recovering and only past the RptSeq the snapshot already
    // carried, so per-instrument LastMsgSeqNumProcessed is honoured without
    // tracking a cursor per instrument.
    builder_.set_apply_mode(ApplyMode::RecoveryReplay);
    buffer_.for_each([this](const PacketBuffer::Entry& entry) {
      ++stats_.replayed;
      process_packet(entry.seq, entry.bytes, entry.rx_ts);
    });
    builder_.set_apply_mode(ApplyMode::Live);
    buffer_.clear();

    // Instruments that never appeared on the loop had no activity this period.
    // They are not missing — they are empty. Only the cycle-completion predicate
    // can tell the difference, and getting it wrong means waiting forever.
    builder_.books().for_each([](SecurityId, InstrumentBook& book) {
      if (book.state == BookState::Recovering || book.state == BookState::Unknown) {
        book.state = BookState::Live;
      }
    });

    ++stats_.snapshot_cycles;
    apply(fsm_.on_snapshot_cycle_complete(now), now);
  }

  // ─── Definition channel ────────────────────────────────────────────

  void on_definition_datagram(const RawPacket& raw) noexcept {
    const auto pkt = PacketView::decode(raw.bytes);
    if (!pkt) {
      ++stats_.packet_rejects;
      return;
    }
    definition_line_.accept(pkt->header.seq, raw.bytes, raw.rx_ts, raw.line);

    while (const auto ordered = definition_line_.next()) {
      for_each_message(ordered->bytes, [&](const MessageView& msg) {
        const auto outcome = builder_.on_message(msg, ordered->seq, ordered->rx_ts);
        if (!outcome) {
          ++stats_.message_errors;
          return;
        }
        if (!outcome->definition) return;
        if (fsm_.state() != FeedState::AwaitingDefinitions) return;
        if (fsm_.definition_cycle().observe(*outcome->definition)) {
          apply(fsm_.on_definitions_complete(ordered->rx_ts), ordered->rx_ts);
        }
      });
    }
  }

  // ─── TCP replay ────────────────────────────────────────────────────

  void on_replay_datagram(const RawPacket& raw) noexcept {
    // Recovered packets carry the incremental sequence space and bypass A/B
    // arbitration entirely — there is only one copy of them.
    const auto pkt = PacketView::decode(raw.bytes);
    if (!pkt) {
      ++stats_.packet_rejects;
      return;
    }
    process_packet(pkt->header.seq, raw.bytes, raw.rx_ts);

    if (fsm_.state() == FeedState::ReplaySync && pkt->header.seq + 1 >= incremental_.expected()) {
      apply(fsm_.on_replay_complete(raw.rx_ts), raw.rx_ts);
    }
  }

  // ─── Shared ────────────────────────────────────────────────────────

  template <class Fn>
  void for_each_message(std::span<const std::byte> datagram, Fn&& fn) noexcept {
    const auto pkt = PacketView::decode(datagram);
    if (!pkt) {
      ++stats_.packet_rejects;
      return;
    }
    if (pkt->is_heartbeat()) {
      ++stats_.heartbeats;
      return;
    }
    auto framer = pkt->framer();
    while (true) {
      const auto msg = framer.next();
      if (!msg) {
        if (msg.error() != DecodeError::EndOfPacket) ++stats_.framing_errors;
        return;
      }
      fn(*msg);
    }
  }

  void process_packet(SeqNum seq, std::span<const std::byte> datagram, Timestamp rx_ts) noexcept {
    for_each_message(datagram, [&](const MessageView& msg) {
      const auto outcome = builder_.on_message(msg, seq, rx_ts);
      if (!outcome) {
        ++stats_.message_errors;
        return;
      }
      if (outcome->reset) handle_reset(*outcome->reset, rx_ts);
      if (outcome->definition && fsm_.state() == FeedState::AwaitingDefinitions &&
          fsm_.definition_cycle().observe(*outcome->definition)) {
        apply(fsm_.on_definitions_complete(rx_ts), rx_ts);
      }
    });
  }

  void handle_reset(const ChannelResetMsg& reset, Timestamp rx_ts) noexcept {
    ++stats_.resets;
    // Scope is the venue's call. Intraday resets usually preserve definitions;
    // a period boundary may invalidate the SecurityID namespace outright.
    builder_.apply_reset_scope(V::reset_scope(reset.kind));
    incremental_.reset();
    snapshot_line_.reprime();
    definition_line_.reprime();
    buffer_.clear();
    apply(fsm_.on_channel_reset(rx_ts), rx_ts);
  }

  void apply(const RecoveryDirective& directive, Timestamp now) noexcept {
    buffering_ = directive.buffer_incrementals;
    builder_.set_publishing(directive.publish);

    if (directive.clear_books) {
      // Natural refresh: empty the affected books and keep applying
      // incrementals, marked Degraded, while real recovery runs underneath.
      builder_.clear_books();
      builder_.set_all_states(BookState::Degraded);
    }
    if (directive.restart_snapshot) {
      buffer_.clear();
      // The snapshot channel is ignored entirely outside SnapshotSync, so its
      // arbiter cursor is frozen wherever the previous join left it. Rejoining
      // without repriming means every snapshot packet reads as window-blown, the
      // arbiter emits nothing, and the cycle never completes — the feed sits in
      // SnapshotSync forever while the loop it needs streams past untouched.
      snapshot_line_.reprime();
      fsm_.snapshot_cycle().begin(now);
    }

    if (auto change = fsm_.take_transition()) {
      change->rx_ts = now;
      (*sink_)(NormalizedEvent{*change});
    }
  }

  TransportT*     transport_{nullptr};
  Sink*           sink_{nullptr};
  Builder         builder_;
  RecoveryFsm<V>  fsm_;
  PacketBuffer    buffer_;
  Arbiter         incremental_{};
  Arbiter         snapshot_line_{};
  Arbiter         definition_line_{};
  HandlerStats    stats_{};
  HandlerConfig   config_{};
  Timestamp       now_{0};
  bool            buffering_{false};
  bool            deferred_start_{false};
  bool            stale_{false};
};

}  // namespace rfeed
