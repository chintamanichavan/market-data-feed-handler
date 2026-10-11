#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "rfeed/book_mbp.hpp"
#include "rfeed/group.hpp"
#include "rfeed/instrument.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/normalized.hpp"
#include "rfeed/packet.hpp"
#include "rfeed/security_map.hpp"
#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed {

struct BuilderStats {
  std::uint64_t messages{0};
  std::uint64_t unknown_templates{0};
  std::uint64_t schema_rejects{0};
  std::uint64_t decode_errors{0};
  std::uint64_t entries_applied{0};
  std::uint64_t entries_clamped{0};
  std::uint64_t desyncs{0};
  std::uint64_t rpt_seq_gaps{0};
  std::uint64_t snapshots_applied{0};
  std::uint64_t definitions{0};
  std::uint64_t trades{0};
  std::uint64_t status_changes{0};
  std::uint64_t resets{0};
  std::uint64_t books_published{0};
  std::uint64_t table_full{0};
  std::uint64_t replay_skipped{0};
};

// How incoming entries are applied.
//
// RecoveryReplay is the mode used when buffered incrementals are replayed on top
// of a freshly-installed snapshot. It applies an entry only to instruments still
// marked Recovering, and only when the entry's RptSeq is past what the snapshot
// already contained — otherwise the replay double-applies updates the snapshot
// already reflects and the book ends up silently wrong.
enum class ApplyMode : std::uint8_t { Live, RecoveryReplay };

// What a message turned out to be, plus anything the recovery FSM needs from it.
struct MessageOutcome {
  LogicalMessage                 logical{LogicalMessage::Unknown};
  std::optional<ChannelResetMsg> reset{};
  std::optional<SnapshotHeader>  snapshot{};
  std::optional<InstrumentDefinition> definition{};
};

// Decodes messages and maintains one MBP book per instrument.
//
// Templated on the venue so the hot path devirtualizes and unsupported features
// compile away. No venue-specific types appear in this header: everything
// arrives through V::Codec and V::classify.
template <VenuePolicy V, EventSink Sink>
class BookBuilder {
 public:
  using Codec = typename V::Codec;
  using Group = GroupReader<V::kGroupCountBytes>;

  BookBuilder(ChannelId channel, Sink& sink, std::size_t capacity = 4096)
      : channel_{channel}, sink_{&sink}, instruments_{capacity}, books_{capacity} {}

  [[nodiscard]] std::expected<MessageOutcome, DecodeError> on_message(const MessageView& msg,
                                                                      SeqNum    seq,
                                                                      Timestamp rx_ts) noexcept {
    ++stats_.messages;

    if (msg.hdr.schema_id != V::kSchemaId) {
      ++stats_.schema_rejects;
      return std::unexpected{DecodeError::UnsupportedSchema};
    }
    if (msg.hdr.version < V::kMinSupportedVersion) {
      ++stats_.schema_rejects;
      return std::unexpected{DecodeError::UnsupportedVersion};
    }

    const auto logical = V::classify(msg.hdr.template_id);
    ByteReader in{msg.body};

    switch (logical) {
      case LogicalMessage::IncrementalBook:      return on_incremental(msg, in, seq, rx_ts);
      case LogicalMessage::SnapshotFullRefresh:  return on_snapshot(msg, in);
      case LogicalMessage::InstrumentDefinition: return on_definition(in, rx_ts);
      case LogicalMessage::SecurityStatus:       return on_status(in, rx_ts);
      case LogicalMessage::ChannelReset:         return on_reset(in);
      case LogicalMessage::Heartbeat:            return MessageOutcome{logical, {}, {}, {}};
      case LogicalMessage::Unknown:
        // Forward compatibility: an unrecognised template is skipped, not fatal.
        // Venues add messages between schema releases.
        ++stats_.unknown_templates;
        return MessageOutcome{logical, {}, {}, {}};
    }
    return MessageOutcome{logical, {}, {}, {}};
  }

  // ─── State the recovery FSM drives ─────────────────────────────────

  void set_publishing(bool on) noexcept { publishing_ = on; }
  [[nodiscard]] bool publishing() const noexcept { return publishing_; }

  void set_apply_mode(ApplyMode mode) noexcept { mode_ = mode; }
  [[nodiscard]] ApplyMode apply_mode() const noexcept { return mode_; }

  void set_state(SecurityId id, BookState state) noexcept {
    if (auto* book = books_.find(id)) book->state = state;
  }

  void set_all_states(BookState state) noexcept {
    books_.for_each([state](SecurityId, InstrumentBook& b) { b.state = state; });
  }

  // Clears book contents. Definitions survive or not per the venue's reset scope.
  void clear_books() noexcept {
    books_.for_each([](SecurityId, InstrumentBook& b) {
      b.clear();
      b.state = BookState::Unknown;
    });
    pending_count_ = 0;
  }

  void apply_reset_scope(const ResetScope& scope) noexcept {
    ++stats_.resets;
    if (scope.books) clear_books();
    if (scope.definitions) instruments_.clear();
    // A venue that reassigns SecurityIDs across periods invalidates every key we
    // hold, so the book table itself has to go, not just its contents.
    if (scope.security_id_namespace) books_.clear();
  }

  [[nodiscard]] InstrumentBook* book(SecurityId id) noexcept { return books_.find(id); }
  [[nodiscard]] const InstrumentBook* book(SecurityId id) const noexcept { return books_.find(id); }
  [[nodiscard]] SecurityMap<InstrumentBook>& books() noexcept { return books_; }
  [[nodiscard]] const InstrumentStore& instruments() const noexcept { return instruments_; }
  [[nodiscard]] InstrumentStore& instruments() noexcept { return instruments_; }
  [[nodiscard]] const BuilderStats& stats() const noexcept { return stats_; }
  [[nodiscard]] ChannelId channel() const noexcept { return channel_; }

  // Publishes any instruments touched by an event that never terminated. Used
  // when a packet boundary or recovery transition interrupts an event.
  void flush(Timestamp rx_ts, SeqNum seq) noexcept { publish_pending(rx_ts, seq, true); }

 private:
  // An exchange event rarely touches more than a handful of instruments; if one
  // does, we flush early rather than grow.
  static constexpr std::size_t kMaxTouched = 128;

  [[nodiscard]] std::expected<MessageOutcome, DecodeError> on_incremental(const MessageView& msg,
                                                                          ByteReader&        in,
                                                                          SeqNum             seq,
                                                                          Timestamp rx_ts) noexcept {
    const auto header = Codec::incremental_header(in);
    if (!header) return fail(header.error());

    if (const auto seek = seek_past_block(in, msg.hdr.block_length); !seek) return fail(seek.error());

    auto group = Group::decode(in);
    if (!group) return fail(group.error());

    while (true) {
      auto entry_reader = group->next();
      if (!entry_reader) {
        if (entry_reader.error() == DecodeError::EndOfGroup) break;
        return fail(entry_reader.error());
      }
      const auto entry = Codec::md_entry(*entry_reader);
      if (!entry) return fail(entry.error());
      apply_entry(*entry, header->transact_time, rx_ts, seq);
    }

    // Apply every update in the event, then publish once. Publishing mid-event
    // exposes transiently crossed and nonsensical books.
    if (header->end_of_event) publish_pending(rx_ts, seq, true);

    return MessageOutcome{LogicalMessage::IncrementalBook, {}, {}, {}};
  }

  void apply_entry(const MdEntry& e, Timestamp exch_ts, Timestamp rx_ts, SeqNum seq) noexcept {
    if (mode_ == ApplyMode::RecoveryReplay) {
      const auto* existing = books_.find(e.security);
      // Only instruments still recovering want this, and only for updates the
      // snapshot did not already contain.
      if (existing == nullptr || existing->state != BookState::Recovering) return;
      if (existing->rpt_seq_primed && e.rpt_seq <= existing->last_rpt_seq) {
        ++stats_.replay_skipped;
        return;
      }
      if (e.type == EntryType::Trade) return;  // trades would be published twice
    }

    if (e.type == EntryType::Trade) {
      ++stats_.trades;
      emit(TradeTick{.key        = key_of(e.security),
                     .px         = e.px,
                     .size       = e.size,
                     .num_orders = e.num_orders,
                     .exch_ts    = exch_ts,
                     .rx_ts      = rx_ts,
                     .seq        = seq});
      return;
    }

    auto* book = books_.emplace(e.security);
    if (book == nullptr) {
      ++stats_.table_full;
      return;
    }

    // Per-instrument RptSeq must advance by exactly one. A jump here while the
    // packet sequence is contiguous means a decoder bug or a venue anomaly, not
    // a network gap — which is why this check is separate from the arbiter's.
    if (book->rpt_seq_primed && e.rpt_seq != book->last_rpt_seq + 1) {
      ++stats_.rpt_seq_gaps;
      book->state = BookState::Degraded;
    }
    book->last_rpt_seq   = e.rpt_seq;
    book->rpt_seq_primed = true;

    switch (book->book_for(e.type).apply(e)) {
      case ApplyResult::Applied: ++stats_.entries_applied; break;
      case ApplyResult::Clamped: ++stats_.entries_clamped; break;
      case ApplyResult::Desync:
        ++stats_.desyncs;
        // Positional state disagrees with the venue. The book is no longer
        // authoritative and must stop claiming to be.
        book->state = BookState::Degraded;
        break;
    }

    touch(e.security, exch_ts);
  }

  [[nodiscard]] std::expected<MessageOutcome, DecodeError> on_snapshot(
      const MessageView& msg, ByteReader& in) noexcept {
    const auto header = Codec::snapshot_header(in);
    if (!header) return fail(header.error());

    if (const auto seek = seek_past_block(in, msg.hdr.block_length); !seek) return fail(seek.error());

    auto group = Group::decode(in);
    if (!group) return fail(group.error());

    auto* book = books_.emplace(header->security);
    if (book == nullptr) {
      ++stats_.table_full;
      return MessageOutcome{LogicalMessage::SnapshotFullRefresh, {}, *header, {}};
    }

    // A snapshot is absolute: everything prior is replaced, not merged.
    book->clear();
    if (const auto* def = instruments_.find(header->security)) book->set_depth(def->market_depth);

    while (true) {
      auto entry_reader = group->next();
      if (!entry_reader) {
        if (entry_reader.error() == DecodeError::EndOfGroup) break;
        return fail(entry_reader.error());
      }
      const auto entry = Codec::snapshot_entry(*entry_reader);
      if (!entry) return fail(entry.error());
      if (!is_book_entry(entry->type)) continue;

      const auto side = is_bid(entry->type) ? Side::Bid : Side::Ask;
      book->book_for(entry->type)
          .install(side, entry->price_level,
                   Level{entry->px, entry->size,
                         static_cast<std::uint32_t>(std::max(entry->num_orders, 0))});
    }

    book->last_rpt_seq   = header->rpt_seq;
    book->rpt_seq_primed = true;
    book->status         = header->status;
    // Deliberately NOT marked Live here: buffered incrementals past
    // LastMsgSeqNumProcessed still have to be replayed on top. That is the
    // recovery FSM's call, not the builder's.
    ++stats_.snapshots_applied;

    return MessageOutcome{LogicalMessage::SnapshotFullRefresh, {}, *header, {}};
  }

  [[nodiscard]] std::expected<MessageOutcome, DecodeError> on_definition(ByteReader& in,
                                                                          Timestamp rx_ts) noexcept {
    const auto def = Codec::instrument_definition(in);
    if (!def) return fail(def.error());

    instruments_.upsert(*def);
    if (auto* book = books_.emplace(def->security)) {
      book->set_depth(def->market_depth);
    } else {
      ++stats_.table_full;
    }
    ++stats_.definitions;

    emit(InstrumentUpdate{.key   = key_of(def->security),
                          .def   = instruments_.find(def->security),
                          .rx_ts = rx_ts});

    return MessageOutcome{LogicalMessage::InstrumentDefinition, {}, {}, *def};
  }

  [[nodiscard]] std::expected<MessageOutcome, DecodeError> on_status(ByteReader& in,
                                                                      Timestamp rx_ts) noexcept {
    const auto status = Codec::security_status(in);
    if (!status) return fail(status.error());

    auto book_state = BookState::Unknown;
    if (auto* book = books_.emplace(status->security)) {
      book->status = status->status;
      if (status->status == TradingStatus::Halted || status->status == TradingStatus::PreOpen) {
        book->state = BookState::Halted;
      } else if (book->state == BookState::Halted) {
        book->state = BookState::Live;
      }
      book_state = book->state;
    }
    ++stats_.status_changes;

    emit(StatusChange{.key        = key_of(status->security),
                      .status     = status->status,
                      .book_state = book_state,
                      .exch_ts    = status->transact_time,
                      .rx_ts      = rx_ts});

    return MessageOutcome{LogicalMessage::SecurityStatus, {}, {}, {}};
  }

  [[nodiscard]] std::expected<MessageOutcome, DecodeError> on_reset(ByteReader& in) noexcept {
    const auto reset = Codec::channel_reset(in);
    if (!reset) return fail(reset.error());
    // Scope is applied by the FSM, which also has to reset the arbiter's
    // sequence space. Mishandling this silently corrupts every book on the
    // channel with no error raised anywhere, so it is never handled implicitly.
    return MessageOutcome{LogicalMessage::ChannelReset, *reset, {}, {}};
  }

  [[nodiscard]] SecurityKey key_of(SecurityId id) const noexcept {
    return SecurityKey{channel_, id};
  }

  void touch(SecurityId id, Timestamp exch_ts) noexcept {
    pending_exch_ts_ = exch_ts;
    for (std::size_t i = 0; i < pending_count_; ++i) {
      if (pending_[i] == id) return;
    }
    if (pending_count_ == kMaxTouched) {
      publish_pending(pending_rx_ts_, pending_seq_, false);
    }
    pending_[pending_count_++] = id;
  }

  void publish_pending(Timestamp rx_ts, SeqNum seq, bool end_of_event) noexcept {
    for (std::size_t i = 0; i < pending_count_; ++i) {
      const auto  id   = pending_[i];
      const auto* book = books_.find(id);
      if (book == nullptr) continue;
      ++stats_.books_published;
      emit(BookUpdate{.key          = key_of(id),
                      .book         = book,
                      .state        = book->state,
                      .exch_ts      = pending_exch_ts_,
                      .rx_ts        = rx_ts,
                      .seq          = seq,
                      .rpt_seq      = book->last_rpt_seq,
                      .end_of_event = end_of_event});
    }
    pending_count_ = 0;
    pending_rx_ts_ = rx_ts;
    pending_seq_   = seq;
  }

  void emit(const NormalizedEvent& event) noexcept {
    if (publishing_) (*sink_)(event);
  }

  [[nodiscard]] std::unexpected<DecodeError> fail(DecodeError e) noexcept {
    ++stats_.decode_errors;
    return std::unexpected{e};
  }

  ChannelId                            channel_{};
  Sink*                                sink_{nullptr};
  InstrumentStore                      instruments_;
  SecurityMap<InstrumentBook>          books_;
  BuilderStats                         stats_{};
  std::array<SecurityId, kMaxTouched>  pending_{};
  std::size_t                          pending_count_{0};
  Timestamp                            pending_exch_ts_{0};
  Timestamp                            pending_rx_ts_{0};
  SeqNum                               pending_seq_{0};
  bool                                 publishing_{true};
  ApplyMode                            mode_{ApplyMode::Live};
};

}  // namespace rfeed
