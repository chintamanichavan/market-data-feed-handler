#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed {

// Venue-neutral decoded messages. Every venue's codec produces these, so
// nothing downstream of decode knows which venue it is looking at.

// Which of these a venue actually supports is a policy question — a venue
// without DeleteThru/DeleteFrom/Overlay simply never emits them, and the book
// builder's handling of them is dead code the compiler strips.
enum class UpdateAction : std::uint8_t {
  New,
  Change,
  Delete,
  DeleteThru,  // delete levels 1..n
  DeleteFrom,  // delete levels n..end
  Overlay,     // replace in place without shifting
  Unknown,
};

enum class EntryType : std::uint8_t {
  Bid,
  Offer,
  ImpliedBid,
  ImpliedOffer,
  Trade,
  Unknown,
};

enum class TradingStatus : std::uint8_t { Unknown, PreOpen, Open, Halted, Paused, Closed };

[[nodiscard]] constexpr bool is_bid(EntryType t) noexcept {
  return t == EntryType::Bid || t == EntryType::ImpliedBid;
}

[[nodiscard]] constexpr bool is_implied(EntryType t) noexcept {
  return t == EntryType::ImpliedBid || t == EntryType::ImpliedOffer;
}

[[nodiscard]] constexpr bool is_book_entry(EntryType t) noexcept {
  return t != EntryType::Trade && t != EntryType::Unknown;
}

// One entry of an incremental refresh. `price_level` is 1-based and is the
// address of the update — MBP updates are positional, not keyed by price.
struct MdEntry {
  Mantissa      px{};
  std::int64_t  size{};
  SecurityId    security{};
  RptSeq        rpt_seq{};
  std::int32_t  num_orders{};
  std::uint8_t  price_level{};
  UpdateAction  action{UpdateAction::Unknown};
  EntryType     type{EntryType::Unknown};
};

struct IncrementalHeader {
  Timestamp transact_time{};
  // Apply every update in an event, then publish once. Publishing mid-event
  // exposes transiently crossed books.
  bool end_of_event{false};
};

struct SnapshotHeader {
  // The incremental-feed packet sequence number this snapshot reflects. This is
  // the join key: buffered incrementals past it must be replayed on top.
  SeqNum        last_msg_seq_num_processed{};
  std::uint32_t tot_num_reports{};
  SecurityId    security{};
  RptSeq        rpt_seq{};
  Timestamp     transact_time{};
  TradingStatus status{TradingStatus::Unknown};
};

struct SnapshotEntry {
  Mantissa     px{};
  std::int64_t size{};
  std::int32_t num_orders{};
  std::uint8_t price_level{};
  EntryType    type{EntryType::Unknown};
};

inline constexpr std::size_t kMaxSymbolLen = 20;

struct InstrumentDefinition {
  SecurityId                        security{};
  std::array<char, kMaxSymbolLen>   symbol{};
  Mantissa                          min_price_increment{};
  std::uint32_t                     tot_num_reports{};
  std::uint8_t                      market_depth{};
  // Some products quote at a finer granularity than min_price_increment
  // (spreads, implieds). 1 means "ticks are the finest addressable price".
  std::uint8_t                      sub_tick_divisor{1};

  [[nodiscard]] std::string_view symbol_view() const noexcept {
    std::size_t n = 0;
    while (n < symbol.size() && symbol[n] != '\0' && symbol[n] != ' ') ++n;
    return std::string_view{symbol.data(), n};
  }
};

struct SecurityStatusMsg {
  SecurityId    security{};
  Timestamp     transact_time{};
  TradingStatus status{TradingStatus::Unknown};
};

struct ChannelResetMsg {
  Timestamp transact_time{};
  ResetKind kind{ResetKind::Intraday};
};

}  // namespace rfeed
