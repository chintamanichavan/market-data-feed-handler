#include "rfeed/book_mbp.hpp"

#include <algorithm>

namespace rfeed {

void MbpBook::install(Side side, std::uint8_t level, const Level& value) noexcept {
  auto& levels = (side == Side::Bid) ? bids_ : asks_;
  auto& n      = (side == Side::Bid) ? n_bids_ : n_asks_;

  if (level == 0 || level > depth_) return;
  const auto idx = static_cast<std::size_t>(level - 1);
  levels[idx]    = value;
  n              = std::max(n, level);
}

ApplyResult MbpBook::apply(const MdEntry& e) noexcept {
  if (!is_book_entry(e.type)) return ApplyResult::Applied;  // trades are not book state
  auto& levels = is_bid(e.type) ? bids_ : asks_;
  auto& n      = is_bid(e.type) ? n_bids_ : n_asks_;
  return apply_to(levels, n, e);
}

ApplyResult MbpBook::apply_to(std::array<Level, kMaxDepth>& levels, std::uint8_t& n,
                              const MdEntry& e) noexcept {
  const auto level = e.price_level;
  if (level == 0) return ApplyResult::Desync;

  // Beyond the instrument's published depth the venue simply stops reporting;
  // dropping is correct, not a loss.
  //
  // Range actions are exempt: DeleteThru/DeleteFrom address a SPAN, not a single
  // level, so one past the depth still means "and everything before/after it".
  // Clamping them would leave levels in the book that the venue just said are
  // gone — a stale book that reports itself healthy.
  const bool is_range = e.action == UpdateAction::DeleteThru ||
                        e.action == UpdateAction::DeleteFrom;
  if (level > depth_ && !is_range) return ApplyResult::Clamped;

  const auto idx      = static_cast<std::size_t>(level - 1);
  const auto incoming = Level{e.px, e.size, static_cast<std::uint32_t>(std::max(e.num_orders, 0))};

  switch (e.action) {
    case UpdateAction::New: {
      // Insert at idx, pushing everything below down one. A level falling off
      // the bottom is expected: the venue only publishes `depth_` levels.
      if (level > n + 1) return ApplyResult::Desync;
      const auto last = static_cast<std::size_t>(std::min<std::uint8_t>(n, depth_ - 1));
      for (std::size_t i = last; i > idx; --i) levels[i] = levels[i - 1];
      levels[idx] = incoming;
      if (n < depth_) ++n;
      return ApplyResult::Applied;
    }

    case UpdateAction::Change: {
      if (level > n) return ApplyResult::Desync;
      // A price mismatch means our positional state disagrees with the venue's.
      // That is a desync signal, not a benign correction — treating it as one
      // is how a book stays subtly wrong for the rest of the session.
      if (levels[idx].px != e.px) return ApplyResult::Desync;
      levels[idx] = incoming;
      return ApplyResult::Applied;
    }

    case UpdateAction::Delete: {
      if (level > n) return ApplyResult::Desync;
      for (std::size_t i = idx; i + 1 < n; ++i) levels[i] = levels[i + 1];
      --n;
      levels[n] = Level{};
      return ApplyResult::Applied;
    }

    case UpdateAction::DeleteThru: {
      // Remove levels 1..level inclusive.
      if (level > n) {
        n = 0;
        return ApplyResult::Applied;
      }
      const auto remaining = static_cast<std::size_t>(n) - level;
      for (std::size_t i = 0; i < remaining; ++i) levels[i] = levels[i + level];
      n = static_cast<std::uint8_t>(remaining);
      for (std::size_t i = n; i < depth_; ++i) levels[i] = Level{};
      return ApplyResult::Applied;
    }

    case UpdateAction::DeleteFrom: {
      // Remove levels level..end inclusive.
      if (level > n) return ApplyResult::Applied;
      for (std::size_t i = idx; i < n; ++i) levels[i] = Level{};
      n = static_cast<std::uint8_t>(level - 1);
      return ApplyResult::Applied;
    }

    case UpdateAction::Overlay: {
      // Replace in place without shifting. Unlike Change, the price may differ.
      if (level > n + 1) return ApplyResult::Desync;
      levels[idx] = incoming;
      if (level > n) n = level;
      return ApplyResult::Applied;
    }

    case UpdateAction::Unknown:
      return ApplyResult::Desync;
  }

  return ApplyResult::Desync;
}

}  // namespace rfeed
