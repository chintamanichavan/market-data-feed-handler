#pragma once

// Rate-limit policy for the TCP replay service, separated from the socket.
//
// Venues police replay hard. Exceeding a limit gets the session dropped, the
// account throttled, or a phone call — and the failure mode is vicious: a gap
// triggers a replay, the replay is refused, the refusal is treated as another
// failure, and the handler hammers the venue exactly when the feed is already
// unhealthy. So the budget is enforced HERE, before a socket is touched, and it
// is pure so the limits can be tested without a network.

#include <cstdint>
#include <expected>
#include <string_view>

#include "rfeed/line_arbiter.hpp"
#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed {

enum class ReplayReject : std::uint8_t {
  RangeTooLarge,     // more packets than the venue serves in one request
  RangeEmpty,        // unbounded gap; there is nothing specific to ask for
  TooManyInFlight,   // venue permits fewer concurrent requests
  DailyQuotaSpent,
  Cooldown,          // asked again too soon after the last request
};

[[nodiscard]] constexpr std::string_view to_string(ReplayReject r) noexcept {
  switch (r) {
    case ReplayReject::RangeTooLarge:   return "RangeTooLarge";
    case ReplayReject::RangeEmpty:      return "RangeEmpty";
    case ReplayReject::TooManyInFlight: return "TooManyInFlight";
    case ReplayReject::DailyQuotaSpent: return "DailyQuotaSpent";
    case ReplayReject::Cooldown:        return "Cooldown";
  }
  return "Unknown";
}

struct ReplayLimiterStats {
  std::uint32_t requests_today{0};
  std::uint32_t in_flight{0};
  std::uint64_t granted{0};
  std::uint64_t rejected{0};
};

// Enforces V::kReplayLimits. Every clock is passed in, so a day boundary or a
// cooldown expiry is a test assertion rather than a sleep.
template <VenuePolicy V>
class ReplayLimiter {
 public:
  [[nodiscard]] std::expected<void, ReplayReject> check(const GapRange& gap,
                                                        Timestamp now) const noexcept {
    // An unbounded gap has no honest request to make. Asking for "everything
    // from here" is how a session gets terminated.
    if (gap.end + 1 == gap.begin || gap.count() == 0) return std::unexpected{ReplayReject::RangeEmpty};
    if (gap.count() > V::kReplayLimits.max_packets_per_request) {
      return std::unexpected{ReplayReject::RangeTooLarge};
    }
    if (in_flight_ >= V::kReplayLimits.max_concurrent_requests) {
      return std::unexpected{ReplayReject::TooManyInFlight};
    }
    if (requests_today_ >= V::kReplayLimits.max_requests_per_day) {
      return std::unexpected{ReplayReject::DailyQuotaSpent};
    }
    if (last_request_ != 0 && now - last_request_ < cooldown_ns()) {
      return std::unexpected{ReplayReject::Cooldown};
    }
    return {};
  }

  // Records a granted request. Separate from check() so a caller can ask
  // "would this be allowed?" without spending budget.
  [[nodiscard]] std::expected<void, ReplayReject> acquire(const GapRange& gap,
                                                          Timestamp now) noexcept {
    if (const auto ok = check(gap, now); !ok) {
      ++stats_.rejected;
      return ok;
    }
    ++in_flight_;
    ++requests_today_;
    ++stats_.granted;
    last_request_ = now;
    return {};
  }

  void release() noexcept {
    if (in_flight_ > 0) --in_flight_;
  }

  // The venue's daily counter resets on its own schedule, not at local midnight.
  // Made explicit rather than inferred from a clock.
  void new_trading_day() noexcept {
    requests_today_ = 0;
    last_request_   = 0;
  }

  [[nodiscard]] ReplayLimiterStats stats() const noexcept {
    auto out           = stats_;
    out.requests_today = requests_today_;
    out.in_flight      = in_flight_;
    return out;
  }

  [[nodiscard]] std::uint32_t in_flight() const noexcept { return in_flight_; }
  [[nodiscard]] std::uint32_t requests_today() const noexcept { return requests_today_; }

  // How long until a cooldown-rejected request would be permitted.
  [[nodiscard]] Timestamp retry_after(Timestamp now) const noexcept {
    if (last_request_ == 0) return 0;
    const auto ready = last_request_ + cooldown_ns();
    return now >= ready ? 0 : ready - now;
  }

 private:
  static constexpr Timestamp cooldown_ns() noexcept {
    return static_cast<Timestamp>(V::kReplayLimits.cooldown.count()) * 1'000'000;
  }

  ReplayLimiterStats stats_{};
  Timestamp          last_request_{0};
  std::uint32_t      requests_today_{0};
  std::uint32_t      in_flight_{0};
};

}  // namespace rfeed
