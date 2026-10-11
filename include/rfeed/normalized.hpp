#pragma once

#include <concepts>
#include <cstdint>
#include <variant>

#include "rfeed/book_mbp.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

// The handler's output. Every book-bearing event carries its BookState, so a
// consumer never has to guess whether what it just received is authoritative.
// This is the most important decision in the whole API: a degraded book that
// claims to be live is worse than no book at all.

struct BookUpdate {
  SecurityKey           key{};
  const InstrumentBook* book{nullptr};  // borrowed; valid only during the callback
  BookState             state{BookState::Unknown};
  Timestamp             exch_ts{};
  Timestamp             rx_ts{};
  SeqNum                seq{};
  RptSeq                rpt_seq{};
  // False for intermediate steps within a multi-message exchange event. Default
  // consumers should act only on true: mid-event books are transiently crossed.
  bool                  end_of_event{true};
};

struct TradeTick {
  SecurityKey  key{};
  Mantissa     px{};
  std::int64_t size{};
  std::int32_t num_orders{};
  Timestamp    exch_ts{};
  Timestamp    rx_ts{};
  SeqNum       seq{};
};

struct StatusChange {
  SecurityKey   key{};
  TradingStatus status{TradingStatus::Unknown};
  BookState     book_state{BookState::Unknown};
  Timestamp     exch_ts{};
  Timestamp     rx_ts{};
};

struct InstrumentUpdate {
  SecurityKey                 key{};
  const InstrumentDefinition* def{nullptr};  // borrowed; valid only during the callback
  Timestamp                   rx_ts{};
};

struct FeedStateChange {
  FeedState from{FeedState::Init};
  FeedState to{FeedState::Init};
  Timestamp rx_ts{};
};

using NormalizedEvent =
    std::variant<BookUpdate, TradeTick, StatusChange, InstrumentUpdate, FeedStateChange>;

// A sink is a template parameter, never a std::function, so the call
// devirtualizes on the hot path. DynamicSink exists for tools and tests where
// latency does not matter.
template <class S>
concept EventSink = requires(S& sink, const NormalizedEvent& event) {
  { sink(event) };
};

}  // namespace rfeed
