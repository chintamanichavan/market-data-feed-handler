#pragma once

#include <compare>
#include <cstdint>
#include <string_view>

namespace rfeed {

// ─── Prices ──────────────────────────────────────────────────────────
//
// MDP 3.0 carries prices as int64 mantissas at a fixed, schema-defined exponent
// (CME uses PRICE9, exponent -9). The mantissa is the canonical internal price:
// it is exactly what the venue sent, and it is always representable.
//
// Conversion to integer ticks is deliberately NOT done here. It is fallible —
// spreads and implied prices routinely land on sub-tick increments, and a naive
// `mantissa / min_price_increment` silently truncates. That conversion lives at
// the normalization boundary and returns std::expected. See instrument.hpp
// (Phase 5).
struct Mantissa {
  std::int64_t raw{};
  auto operator<=>(const Mantissa&) const = default;
};

// Integer-tick price, shaped to mirror rtrade::Price so the adapter is trivial.
// Only ever produced by a checked conversion from Mantissa.
struct Price {
  std::int64_t ticks{};
  auto operator<=>(const Price&) const = default;
};

struct Quantity {
  std::int64_t units{};
  auto operator<=>(const Quantity&) const = default;
};

// ─── Wire identifiers ────────────────────────────────────────────────

using SeqNum     = std::uint32_t;  // packet-level MsgSeqNum, per channel per feed
using RptSeq     = std::uint32_t;  // per-instrument repeat sequence number
using SecurityId = std::int32_t;   // MDP SecurityID
using ChannelId  = std::uint16_t;
using Timestamp  = std::int64_t;   // ns since epoch (matches rtrade::Timestamp)

// SecurityID is unique only within a channel, and on CME it is reassigned every
// week. Never key an instrument on SecurityId alone — always on this pair.
struct SecurityKey {
  ChannelId  channel{};
  SecurityId security{};
  auto operator<=>(const SecurityKey&) const = default;
};

enum class Side : std::uint8_t { Bid, Ask };

// The two byte-identical redundant multicast feeds.
enum class LineId : std::uint8_t { A, B };

// How a packet reached the framer. Replay and Snapshot packets carry their own
// sequence spaces and bypass A/B arbitration entirely.
enum class PacketOrigin : std::uint8_t { Live, Replay, Snapshot };

// ─── State ───────────────────────────────────────────────────────────

// Rides on every published book update. A consumer must never have to guess
// whether the book it just received is authoritative.
enum class BookState : std::uint8_t {
  Unknown,     // no data yet for this instrument
  Recovering,  // snapshot or replay in flight; contents not usable
  Degraded,    // natural-refresh rebuild; incomplete but directionally usable
  Live,        // authoritative
  Stale,       // no updates past the venue's expected cadence
  Halted,      // venue-declared halt / pre-open
};

// Channel-level synchronisation state. Degradation is deliberately NOT a state
// here: natural-refresh degradation runs *concurrently* with snapshot or replay
// recovery, so it is a separate flag on the FSM and a BookState per instrument.
// Folding them into one enum forces a choice the runtime does not have to make.
enum class FeedState : std::uint8_t {
  Init,
  AwaitingDefinitions,
  SnapshotSync,
  ReplaySync,
  Synchronized,
  Failed,
};

[[nodiscard]] constexpr std::string_view to_string(FeedState s) noexcept {
  switch (s) {
    case FeedState::Init:                return "Init";
    case FeedState::AwaitingDefinitions: return "AwaitingDefinitions";
    case FeedState::SnapshotSync:        return "SnapshotSync";
    case FeedState::ReplaySync:          return "ReplaySync";
    case FeedState::Synchronized:        return "Synchronized";
    case FeedState::Failed:              return "Failed";
  }
  return "Unknown";
}

// ─── Decode failures ─────────────────────────────────────────────────

enum class DecodeError : std::uint8_t {
  Truncated,                   // fewer bytes remain than the field needs
  BadMessageSize,              // framed MsgSize below the SBE header size (incl. 0)
  MessageOverrunsPacket,       // framed MsgSize extends past the datagram
  BlockLengthOverrunsMessage,  // SBE root block larger than the framed message
  BadGroupHeader,              // groupSize composite could not be read
  GroupOverrunsMessage,        // blockLength * numInGroup extends past the message
  UnknownTemplate,
  UnsupportedSchema,
  UnsupportedVersion,
  EndOfPacket,  // not a fault: the message region ended cleanly
  EndOfGroup,   // not a fault: the repeating group ended cleanly
};

[[nodiscard]] constexpr std::string_view to_string(DecodeError e) noexcept {
  switch (e) {
    case DecodeError::Truncated:                  return "Truncated";
    case DecodeError::BadMessageSize:             return "BadMessageSize";
    case DecodeError::MessageOverrunsPacket:      return "MessageOverrunsPacket";
    case DecodeError::BlockLengthOverrunsMessage: return "BlockLengthOverrunsMessage";
    case DecodeError::BadGroupHeader:             return "BadGroupHeader";
    case DecodeError::GroupOverrunsMessage:       return "GroupOverrunsMessage";
    case DecodeError::UnknownTemplate:            return "UnknownTemplate";
    case DecodeError::UnsupportedSchema:          return "UnsupportedSchema";
    case DecodeError::UnsupportedVersion:         return "UnsupportedVersion";
    case DecodeError::EndOfPacket:                return "EndOfPacket";
    case DecodeError::EndOfGroup:                 return "EndOfGroup";
  }
  return "Unknown";
}

}  // namespace rfeed
