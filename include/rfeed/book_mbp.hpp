#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

// Deepest book we will hold. MDP venues publish 10 levels for most outrights;
// 32 leaves headroom without making the array cross into a second page.
inline constexpr std::size_t kMaxDepth = 32;

struct Level {
  Mantissa      px{};
  std::int64_t  qty{};
  std::uint32_t order_count{};

  auto operator<=>(const Level&) const = default;
};

enum class ApplyResult : std::uint8_t {
  Applied,
  Clamped,  // update fell past the instrument's depth; correct to drop
  Desync,   // positional update against state we do not have — recover
};

// Market-by-price book for one side-pair of one instrument.
//
// Updates are addressed by 1-based LEVEL INDEX, not by price. That is the whole
// difference from a matching engine's book: there is no order ID, no price
// lookup, and a Change at level 3 presupposes that levels 1 and 2 are already
// correct. It is also why natural refresh is structurally unreliable for MBP —
// after a gap, the positional updates no longer mean what they claim.
//
// Fixed arrays of POD, so a level insert is a short memmove over a couple of
// cache lines. Faster than any tree at this depth, trivially deterministic, and
// it never allocates after construction.
class MbpBook {
 public:
  void set_depth(std::uint8_t depth) noexcept {
    depth_ = depth == 0 ? static_cast<std::uint8_t>(kMaxDepth)
                        : static_cast<std::uint8_t>(std::min<std::size_t>(depth, kMaxDepth));
    n_bids_ = static_cast<std::uint8_t>(std::min<std::size_t>(n_bids_, depth_));
    n_asks_ = static_cast<std::uint8_t>(std::min<std::size_t>(n_asks_, depth_));
  }

  [[nodiscard]] ApplyResult apply(const MdEntry& e) noexcept;

  void clear() noexcept {
    n_bids_ = 0;
    n_asks_ = 0;
  }

  [[nodiscard]] std::span<const Level> bids() const noexcept { return {bids_.data(), n_bids_}; }
  [[nodiscard]] std::span<const Level> asks() const noexcept { return {asks_.data(), n_asks_}; }

  [[nodiscard]] const Level* best_bid() const noexcept {
    return n_bids_ > 0 ? &bids_[0] : nullptr;
  }
  [[nodiscard]] const Level* best_ask() const noexcept {
    return n_asks_ > 0 ? &asks_[0] : nullptr;
  }

  [[nodiscard]] std::uint8_t depth() const noexcept { return depth_; }
  [[nodiscard]] bool empty() const noexcept { return n_bids_ == 0 && n_asks_ == 0; }

  // A book whose best bid is at or above its best ask is either mid-event or
  // wrong. Publishing event-atomically means the second case is a real signal.
  [[nodiscard]] bool crossed() const noexcept {
    return n_bids_ > 0 && n_asks_ > 0 && bids_[0].px.raw >= asks_[0].px.raw;
  }

  [[nodiscard]] bool operator==(const MbpBook& other) const noexcept {
    if (n_bids_ != other.n_bids_ || n_asks_ != other.n_asks_) return false;
    for (std::size_t i = 0; i < n_bids_; ++i) {
      if (bids_[i] != other.bids_[i]) return false;
    }
    for (std::size_t i = 0; i < n_asks_; ++i) {
      if (asks_[i] != other.asks_[i]) return false;
    }
    return true;
  }

  // Snapshot application: levels arrive absolute, not as deltas.
  void install(Side side, std::uint8_t level, const Level& value) noexcept;

 private:
  [[nodiscard]] ApplyResult apply_to(std::array<Level, kMaxDepth>& side, std::uint8_t& n,
                                     const MdEntry& e) noexcept;

  std::array<Level, kMaxDepth> bids_{};
  std::array<Level, kMaxDepth> asks_{};
  std::uint8_t                 n_bids_{0};
  std::uint8_t                 n_asks_{0};
  std::uint8_t                 depth_{static_cast<std::uint8_t>(kMaxDepth)};
};

// Everything held for one instrument.
//
// Outright and implied books are separate instances. Implied bid/offer are
// distinct entry types and consumers frequently want them apart, so they are
// never merged eagerly — merging is lossy and cannot be undone downstream.
struct InstrumentBook {
  MbpBook       outright{};
  MbpBook       implied{};
  RptSeq        last_rpt_seq{0};
  BookState     state{BookState::Unknown};
  TradingStatus status{TradingStatus::Unknown};
  bool          rpt_seq_primed{false};

  void clear() noexcept {
    outright.clear();
    implied.clear();
    last_rpt_seq   = 0;
    rpt_seq_primed = false;
  }

  void set_depth(std::uint8_t depth) noexcept {
    outright.set_depth(depth);
    implied.set_depth(depth);
  }

  [[nodiscard]] MbpBook& book_for(EntryType t) noexcept {
    return is_implied(t) ? implied : outright;
  }
};

}  // namespace rfeed
