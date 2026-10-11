#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "rfeed/transport.hpp"
#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed {

struct ArbiterStats {
  std::uint64_t                emitted{0};
  std::uint64_t                out_of_order{0};
  std::uint64_t                window_blown{0};
  std::array<std::uint64_t, 2> duplicates{};      // indexed by LineId
  std::array<std::uint64_t, 2> first_arrivals{};  // who won the race, per line
  std::array<std::uint64_t, 2> received{};
};

struct GapRange {
  SeqNum begin{};  // first missing sequence number, inclusive
  SeqNum end{};    // last missing, inclusive

  [[nodiscard]] constexpr std::uint32_t count() const noexcept { return end - begin + 1; }
};

// A/B line arbitration for one channel.
//
// Both lines carry byte-identical packets under identical sequence numbers, so
// dedup is by sequence number alone — never by content hash, which is slower and
// no more correct.
//
// Copy policy is deliberate: the in-order path, which is the overwhelming
// majority, is zero-copy and hands back a span pointing straight into the
// transport's receive buffer. Only out-of-order packets are copied into the
// ring, because the transport buffer is recycled on the next poll(). That keeps
// the memcpy confined to the rare case.
//
// A hole is NOT a gap. The other line usually fills it within microseconds, so
// a hole arms a deadline (Venue::kGapDeadline) and only escalates on expiry.
// Escalating eagerly triggers needless snapshot recovery; escalating lazily adds
// latency to real gaps.
template <VenuePolicy V, std::size_t Window = 256>
  requires(std::has_single_bit(Window))
class LineArbiter {
 public:
  struct Packet {
    SeqNum                     seq{};
    std::span<const std::byte> bytes{};
    Timestamp                  rx_ts{};
  };

  // Feeds one datagram in. Emitted packets are then drained with next().
  void accept(SeqNum seq, std::span<const std::byte> bytes, Timestamp rx_ts,
              LineId line) noexcept {
    const auto li = static_cast<std::size_t>(line);
    ++stats_.received[li];
    last_rx_[li] = rx_ts;

    // A live feed is joined mid-stream: the first packet defines where we are.
    if (!primed_) {
      primed_   = true;
      expected_ = seq;
    }

    // Unsigned difference cast to signed handles the 2^32 wrap correctly.
    const auto delta = static_cast<std::int32_t>(seq - expected_);

    if (delta < 0) {
      ++stats_.duplicates[li];
      return;
    }

    if (delta == 0) {
      ++stats_.first_arrivals[li];
      ++stats_.emitted;
      pending_ = Packet{seq, bytes, rx_ts};
      ++expected_;
      return;
    }

    if (static_cast<std::uint32_t>(delta) >= Window) {
      // Too far ahead to buffer. This is an immediate hard gap — there is no
      // point waiting for the other line.
      ++stats_.window_blown;
      hard_gap_ = true;
      return;
    }

    const auto idx = static_cast<std::size_t>(seq) & kMask;
    if (ring_[idx].present && ring_[idx].seq == seq) {
      ++stats_.duplicates[li];
      return;
    }

    ++stats_.first_arrivals[li];
    ++stats_.out_of_order;

    auto& slot = ring_[idx];
    slot.len   = std::min(bytes.size(), kMaxDatagramBytes);
    std::copy_n(bytes.begin(), slot.len, slot.storage.begin());
    slot.rx_ts = rx_ts;
    slot.seq   = seq;
    if (!slot.present) {
      slot.present = true;
      ++buffered_;
    }

    if (buffered_ == 1) gap_deadline_ = rx_ts + deadline_ns();
  }

  // Next in-order packet, or nullopt. Call until empty after every accept().
  [[nodiscard]] std::optional<Packet> next() noexcept {
    if (pending_) {
      const auto p = *pending_;
      pending_.reset();
      return p;
    }

    const auto idx = static_cast<std::size_t>(expected_) & kMask;
    auto&      slot = ring_[idx];
    if (!slot.present || slot.seq != expected_) return std::nullopt;

    slot.present = false;
    --buffered_;
    ++stats_.emitted;
    ++expected_;
    return Packet{slot.seq, std::span<const std::byte>{slot.storage.data(), slot.len}, slot.rx_ts};
  }

  // True once a hole has outlived the venue's gap deadline, or immediately when
  // the reorder window was blown. This is the only signal that should drive the
  // recovery FSM — a hole on its own is not one.
  [[nodiscard]] bool gap_matured(Timestamp now) const noexcept {
    if (hard_gap_) return true;
    return buffered_ > 0 && now >= gap_deadline_;
  }

  // The missing range, for a replay request. Empty end==begin-1 when the hole
  // is unbounded (window blown, nothing buffered beyond it).
  [[nodiscard]] GapRange pending_gap() const noexcept {
    SeqNum probe = expected_;
    for (std::uint32_t i = 0; i < Window; ++i) {
      const auto idx = static_cast<std::size_t>(probe) & kMask;
      if (ring_[idx].present && ring_[idx].seq == probe) {
        return GapRange{expected_, probe - 1};
      }
      ++probe;
    }
    return GapRange{expected_, expected_ - 1};  // unbounded
  }

  // Recovery filled the hole (or we chose to abandon it). Jump the cursor and
  // drop anything now stale.
  void resync_to(SeqNum seq) noexcept {
    expected_ = seq;
    primed_   = true;
    hard_gap_ = false;
    pending_.reset();
    for (auto& slot : ring_) {
      if (slot.present && static_cast<std::int32_t>(slot.seq - expected_) < 0) {
        slot.present = false;
        --buffered_;
      }
    }
    if (buffered_ == 0) gap_deadline_ = 0;
  }

  // Abandon the current position entirely and adopt whatever sequence number
  // arrives next. Used when recovery gives up on an unbounded gap: there is no
  // meaningful cursor to resync to, only "wherever the feed is now".
  void reprime() noexcept {
    for (auto& slot : ring_) slot.present = false;
    buffered_     = 0;
    primed_       = false;
    hard_gap_     = false;
    gap_deadline_ = 0;
    pending_.reset();
  }

  // ChannelReset: the venue restarted the sequence space.
  void reset(SeqNum start = 1) noexcept {
    for (auto& slot : ring_) slot.present = false;
    buffered_     = 0;
    expected_     = start;
    primed_       = true;
    hard_gap_     = false;
    gap_deadline_ = 0;
    pending_.reset();
  }

  // A silent line is the most common production feed incident and must never be
  // a book-correctness event — it is an alert, not a gap.
  [[nodiscard]] bool line_silent(LineId line, Timestamp now,
                                 Timestamp max_silence_ns) const noexcept {
    const auto li = static_cast<std::size_t>(line);
    if (stats_.received[li] == 0) return true;
    return now - last_rx_[li] > max_silence_ns;
  }

  [[nodiscard]] SeqNum       expected() const noexcept { return expected_; }
  [[nodiscard]] std::uint32_t buffered() const noexcept { return buffered_; }
  [[nodiscard]] bool          hard_gap() const noexcept { return hard_gap_; }
  [[nodiscard]] const ArbiterStats& stats() const noexcept { return stats_; }

 private:
  static constexpr std::size_t kMask = Window - 1;

  static constexpr Timestamp deadline_ns() noexcept {
    return static_cast<Timestamp>(V::kGapDeadline.count());
  }

  struct Slot {
    std::array<std::byte, kMaxDatagramBytes> storage{};
    std::size_t                              len{0};
    Timestamp                                rx_ts{0};
    SeqNum                                   seq{0};
    bool                                     present{false};
  };

  std::array<Slot, Window> ring_{};
  std::optional<Packet>    pending_{};
  std::array<Timestamp, 2> last_rx_{};
  ArbiterStats             stats_{};
  Timestamp                gap_deadline_{0};
  SeqNum                   expected_{1};
  std::uint32_t            buffered_{0};
  bool                     primed_{false};
  bool                     hard_gap_{false};
};

}  // namespace rfeed
