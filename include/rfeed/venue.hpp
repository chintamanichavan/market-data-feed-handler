#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include "rfeed/types.hpp"

namespace rfeed {

// What a message *means*, independent of how any venue numbers it.
//
// Template IDs are never hard-coded in the core. Two venues on the same protocol
// version routinely number the same logical message differently, and a single
// venue renumbers across schema versions. Every venue supplies a classify()
// that maps its own IDs onto this enum.
enum class LogicalMessage : std::uint8_t {
  Unknown,
  Heartbeat,
  ChannelReset,
  SecurityStatus,
  IncrementalBook,
  SnapshotFullRefresh,
  InstrumentDefinition,
};

[[nodiscard]] constexpr std::string_view to_string(LogicalMessage m) noexcept {
  switch (m) {
    case LogicalMessage::Unknown:              return "Unknown";
    case LogicalMessage::Heartbeat:            return "Heartbeat";
    case LogicalMessage::ChannelReset:         return "ChannelReset";
    case LogicalMessage::SecurityStatus:       return "SecurityStatus";
    case LogicalMessage::IncrementalBook:      return "IncrementalBook";
    case LogicalMessage::SnapshotFullRefresh:  return "SnapshotFullRefresh";
    case LogicalMessage::InstrumentDefinition: return "InstrumentDefinition";
  }
  return "Unknown";
}

// A channel reset does not always clear the same things. Intraday resets
// typically preserve instrument definitions; a period boundary may additionally
// invalidate the entire SecurityID namespace. Guessing here silently corrupts
// every book on the channel with no error surfaced anywhere.
enum class ResetKind : std::uint8_t { Intraday, PeriodStart };

struct ResetScope {
  bool books{true};
  bool sequence{true};
  bool definitions{false};
  bool security_id_namespace{false};
};

// Exceeding these gets you disconnected, or a phone call from the venue. They
// are enforced in ReplayClient, not left to convention.
struct ReplayLimits {
  std::uint32_t max_packets_per_request{1000};
  std::uint32_t max_concurrent_requests{1};
  std::uint32_t max_requests_per_day{100};
  std::chrono::milliseconds cooldown{1000};
};

// A venue satisfying this concept can be plugged into the core without the core
// containing a single venue conditional. If you find yourself writing
// `if (venue == Cme)` under src/, the behaviour belongs here instead.
template <class V>
concept VenuePolicy = requires {
  { V::kName } -> std::convertible_to<std::string_view>;
  { V::kSchemaId } -> std::convertible_to<std::uint16_t>;
  { V::kMinSupportedVersion } -> std::convertible_to<std::uint16_t>;

  // Price mantissa exponent (CME PRICE9 => -9).
  { V::kPriceExponent } -> std::convertible_to<std::int8_t>;

  // The groupSize composite: blockLength is always uint16, but numInGroup is
  // uint8 on CME and uint16 elsewhere. This single byte is the strongest
  // argument for generating codecs instead of hand-writing them — get it wrong
  // and every repeating group after the first misparses, silently.
  { V::kGroupCountBytes } -> std::convertible_to<std::size_t>;

  // Does an idle-feed heartbeat consume a sequence number? Backwards in either
  // direction produces a permanent phantom gap or a permanent phantom duplicate.
  { V::kHeartbeatConsumesSeqNum } -> std::convertible_to<bool>;

  // How long to wait for the other line to fill a hole before declaring a gap.
  // Too eager triggers needless recovery; too lazy adds latency to real gaps.
  { V::kGapDeadline } -> std::convertible_to<std::chrono::nanoseconds>;

  { V::kReplayLimits } -> std::convertible_to<ReplayLimits>;

  // False when SecurityID is reassigned across periods (CME: weekly).
  { V::kSecurityIdStableAcrossPeriods } -> std::convertible_to<bool>;

  { V::classify(std::uint16_t{}) } -> std::same_as<LogicalMessage>;
  { V::reset_scope(ResetKind{}) } -> std::same_as<ResetScope>;
};

// Shared defaults a venue can inherit and selectively override.
struct VenueDefaults {
  static constexpr std::uint16_t kMinSupportedVersion = 0;
  static constexpr std::int8_t   kPriceExponent = -9;
  static constexpr std::size_t   kGroupCountBytes = 1;
  static constexpr bool          kHeartbeatConsumesSeqNum = false;
  static constexpr std::chrono::nanoseconds kGapDeadline = std::chrono::milliseconds{5};
  static constexpr ReplayLimits  kReplayLimits{};
  static constexpr bool          kSecurityIdStableAcrossPeriods = false;

  static constexpr ResetScope reset_scope(ResetKind kind) noexcept {
    return kind == ResetKind::PeriodStart
               ? ResetScope{true, true, true, true}
               : ResetScope{true, true, false, false};
  }
};

}  // namespace rfeed
