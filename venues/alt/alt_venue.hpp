#pragma once

// Second reference venue. Its only purpose is to be DIFFERENT.
//
// A "venue-neutral" core that has only ever run against one venue is neutral by
// assertion. This one disagrees with venue_ref on every axis the VenuePolicy
// concept exists to abstract:
//
//   - numInGroup is uint16, not uint8  ← the one that silently misparses every
//     repeating group after the first if the core ever assumes a width
//   - different schema id, template ids and block lengths
//   - fields in a different ORDER within each block
//   - EntryType and TradingStatus as dense numeric codes, not FIX ASCII
//   - heartbeats consume a sequence number
//   - a different price exponent
//   - SecurityID is stable across periods, so a period reset must NOT discard
//     the instrument table
//
// If any of that required a change under src/ or include/, the abstraction was
// wrong. It did not.

#include <cstdint>
#include <expected>
#include <string_view>

#include "rfeed/byte_reader.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed::venue_alt {

// Deliberately unlike venue_ref's numbering.
inline constexpr std::uint16_t kTplChannelReset    = 1;
inline constexpr std::uint16_t kTplHeartbeat       = 2;
inline constexpr std::uint16_t kTplSecurityStatus  = 3;
inline constexpr std::uint16_t kTplIncrementalBook = 10;
inline constexpr std::uint16_t kTplSnapshot        = 11;
inline constexpr std::uint16_t kTplInstrumentDef   = 12;

inline constexpr std::uint16_t kBlkChannelReset   = 12;
inline constexpr std::uint16_t kBlkSecurityStatus = 16;
inline constexpr std::uint16_t kBlkIncremental    = 16;
inline constexpr std::uint16_t kBlkSnapshot       = 32;
inline constexpr std::uint16_t kBlkInstrumentDef  = 40;
inline constexpr std::uint16_t kBlkMdEntry        = 32;
inline constexpr std::uint16_t kBlkSnapshotEntry  = 24;

inline constexpr std::uint8_t kEventEndOfEventBit = 0x80;  // high bit, not low

// Dense numeric codes rather than FIX ASCII.
[[nodiscard]] constexpr EntryType to_entry_type(std::uint8_t raw) noexcept {
  switch (raw) {
    case 0:  return EntryType::Bid;
    case 1:  return EntryType::Offer;
    case 2:  return EntryType::Trade;
    case 3:  return EntryType::ImpliedBid;
    case 4:  return EntryType::ImpliedOffer;
    default: return EntryType::Unknown;
  }
}

[[nodiscard]] constexpr std::uint8_t from_entry_type(EntryType t) noexcept {
  switch (t) {
    case EntryType::Bid:          return 0;
    case EntryType::Offer:        return 1;
    case EntryType::Trade:        return 2;
    case EntryType::ImpliedBid:   return 3;
    case EntryType::ImpliedOffer: return 4;
    case EntryType::Unknown:      return 0xFF;
  }
  return 0xFF;
}

[[nodiscard]] constexpr UpdateAction to_update_action(std::uint8_t raw) noexcept {
  switch (raw) {
    case 0:  return UpdateAction::New;
    case 1:  return UpdateAction::Change;
    case 2:  return UpdateAction::Delete;
    case 3:  return UpdateAction::DeleteThru;
    case 4:  return UpdateAction::DeleteFrom;
    case 5:  return UpdateAction::Overlay;
    default: return UpdateAction::Unknown;
  }
}

[[nodiscard]] constexpr std::uint8_t from_update_action(UpdateAction a) noexcept {
  switch (a) {
    case UpdateAction::New:        return 0;
    case UpdateAction::Change:     return 1;
    case UpdateAction::Delete:     return 2;
    case UpdateAction::DeleteThru: return 3;
    case UpdateAction::DeleteFrom: return 4;
    case UpdateAction::Overlay:    return 5;
    case UpdateAction::Unknown:    return 0xFF;
  }
  return 0xFF;
}

[[nodiscard]] constexpr TradingStatus to_trading_status(std::uint8_t raw) noexcept {
  switch (raw) {
    case 1:  return TradingStatus::PreOpen;
    case 2:  return TradingStatus::Open;
    case 3:  return TradingStatus::Halted;
    case 4:  return TradingStatus::Paused;
    case 5:  return TradingStatus::Closed;
    default: return TradingStatus::Unknown;
  }
}

[[nodiscard]] constexpr std::uint8_t from_trading_status(TradingStatus s) noexcept {
  switch (s) {
    case TradingStatus::PreOpen: return 1;
    case TradingStatus::Open:    return 2;
    case TradingStatus::Halted:  return 3;
    case TradingStatus::Paused:  return 4;
    case TradingStatus::Closed:  return 5;
    case TradingStatus::Unknown: return 0;
  }
  return 0;
}

struct AltCodec {
  [[nodiscard]] static std::expected<IncrementalHeader, DecodeError> incremental_header(
      ByteReader& in) noexcept;
  [[nodiscard]] static std::expected<MdEntry, DecodeError> md_entry(ByteReader& in) noexcept;
  [[nodiscard]] static std::expected<SnapshotHeader, DecodeError> snapshot_header(
      ByteReader& in) noexcept;
  [[nodiscard]] static std::expected<SnapshotEntry, DecodeError> snapshot_entry(
      ByteReader& in) noexcept;
  [[nodiscard]] static std::expected<InstrumentDefinition, DecodeError> instrument_definition(
      ByteReader& in) noexcept;
  [[nodiscard]] static std::expected<SecurityStatusMsg, DecodeError> security_status(
      ByteReader& in) noexcept;
  [[nodiscard]] static std::expected<ChannelResetMsg, DecodeError> channel_reset(
      ByteReader& in) noexcept;
};

struct AltVenue : VenueDefaults {
  using Codec = AltCodec;

  static constexpr std::string_view kName = "alt";
  static constexpr std::uint16_t    kSchemaId = 42;
  static constexpr std::uint16_t    kMinSupportedVersion = 3;
  static constexpr std::int8_t      kPriceExponent = -7;
  static constexpr std::size_t      kGroupCountBytes = 2;  // uint16 numInGroup
  static constexpr bool             kHeartbeatConsumesSeqNum = true;
  static constexpr bool             kSecurityIdStableAcrossPeriods = true;
  static constexpr std::chrono::nanoseconds kGapDeadline = std::chrono::milliseconds{2};
  static constexpr ReplayLimits     kReplayLimits{
          .max_packets_per_request = 500,
          .max_concurrent_requests = 2,
          .max_requests_per_day    = 250,
          .cooldown                = std::chrono::milliseconds{250},
  };

  static constexpr LogicalMessage classify(std::uint16_t template_id) noexcept {
    switch (template_id) {
      case kTplChannelReset:    return LogicalMessage::ChannelReset;
      case kTplHeartbeat:       return LogicalMessage::Heartbeat;
      case kTplSecurityStatus:  return LogicalMessage::SecurityStatus;
      case kTplIncrementalBook: return LogicalMessage::IncrementalBook;
      case kTplSnapshot:        return LogicalMessage::SnapshotFullRefresh;
      case kTplInstrumentDef:   return LogicalMessage::InstrumentDefinition;
      default:                  return LogicalMessage::Unknown;
    }
  }

  // SecurityIDs are permanent here, so a period boundary clears books and the
  // sequence space but keeps the instrument table. Getting this backwards on a
  // venue like this throws away every definition at every rollover; getting it
  // backwards on venue_ref keeps stale IDs that now mean different instruments.
  static constexpr ResetScope reset_scope(ResetKind kind) noexcept {
    return kind == ResetKind::PeriodStart ? ResetScope{true, true, false, false}
                                          : ResetScope{true, true, false, false};
  }
};

static_assert(VenuePolicy<AltVenue>);
static_assert(AltVenue::kGroupCountBytes != 1, "the point of this venue is to differ");

}  // namespace rfeed::venue_alt
