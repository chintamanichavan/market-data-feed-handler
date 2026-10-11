// MBP book mechanics and the fallible tick conversion.
//
// The book is addressed by 1-based level index, so every test here is really
// asking the same question: does a positional update land where the venue meant
// it to, and does the book refuse to guess when it cannot know?

#include <gtest/gtest.h>

#include "rfeed/book_mbp.hpp"
#include "rfeed/instrument.hpp"

namespace rfeed {
namespace {

MdEntry bid(std::uint8_t level, UpdateAction action, std::int64_t px, std::int64_t qty,
            std::int32_t orders = 1) {
  return MdEntry{.px          = Mantissa{px},
                 .size        = qty,
                 .security    = 1,
                 .rpt_seq     = 0,
                 .num_orders  = orders,
                 .price_level = level,
                 .action      = action,
                 .type        = EntryType::Bid};
}

MdEntry ask(std::uint8_t level, UpdateAction action, std::int64_t px, std::int64_t qty,
            std::int32_t orders = 1) {
  auto e = bid(level, action, px, qty, orders);
  e.type = EntryType::Offer;
  return e;
}

// Builds a 3-deep bid side: 100@10, 99@20, 98@30.
MbpBook three_bids() {
  MbpBook book;
  book.set_depth(5);
  EXPECT_EQ(book.apply(bid(1, UpdateAction::New, 100, 10)), ApplyResult::Applied);
  EXPECT_EQ(book.apply(bid(2, UpdateAction::New, 99, 20)), ApplyResult::Applied);
  EXPECT_EQ(book.apply(bid(3, UpdateAction::New, 98, 30)), ApplyResult::Applied);
  return book;
}

testing::AssertionResult BidsAre(const MbpBook& book,
                                 std::vector<std::pair<std::int64_t, std::int64_t>> expected) {
  const auto levels = book.bids();
  if (levels.size() != expected.size()) {
    return testing::AssertionFailure()
           << "bid depth " << levels.size() << ", expected " << expected.size();
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (levels[i].px.raw != expected[i].first || levels[i].qty != expected[i].second) {
      return testing::AssertionFailure()
             << "level " << (i + 1) << " is " << levels[i].px.raw << "@" << levels[i].qty
             << ", expected " << expected[i].first << "@" << expected[i].second;
    }
  }
  return testing::AssertionSuccess();
}

// ─── New ─────────────────────────────────────────────────────────────

TEST(MbpBookNew, InsertsAtTopAndPushesDown) {
  auto book = three_bids();
  ASSERT_TRUE(BidsAre(book, {{100, 10}, {99, 20}, {98, 30}}));

  EXPECT_EQ(book.apply(bid(1, UpdateAction::New, 101, 5)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{101, 5}, {100, 10}, {99, 20}, {98, 30}}));
}

TEST(MbpBookNew, InsertsInTheMiddle) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(2, UpdateAction::New, 99, 7)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 7}, {99, 20}, {98, 30}}));
}

TEST(MbpBookNew, AppendsOnePastTheEnd) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(4, UpdateAction::New, 97, 40)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}, {98, 30}, {97, 40}}));
}

TEST(MbpBookNew, MoreThanOnePastTheEndIsDesync) {
  auto book = three_bids();
  // Level 5 when only 3 exist would leave level 4 fabricated. Refuse.
  EXPECT_EQ(book.apply(bid(5, UpdateAction::New, 96, 50)), ApplyResult::Desync);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}, {98, 30}}));
}

TEST(MbpBookNew, PushesTheBottomLevelOffAtFullDepth) {
  MbpBook book;
  book.set_depth(3);
  ASSERT_EQ(book.apply(bid(1, UpdateAction::New, 100, 10)), ApplyResult::Applied);
  ASSERT_EQ(book.apply(bid(2, UpdateAction::New, 99, 20)), ApplyResult::Applied);
  ASSERT_EQ(book.apply(bid(3, UpdateAction::New, 98, 30)), ApplyResult::Applied);

  EXPECT_EQ(book.apply(bid(1, UpdateAction::New, 101, 5)), ApplyResult::Applied);
  // 98 fell off the bottom — the venue only publishes three levels.
  EXPECT_TRUE(BidsAre(book, {{101, 5}, {100, 10}, {99, 20}}));
}

// ─── Change ──────────────────────────────────────────────────────────

TEST(MbpBookChange, UpdatesQuantityInPlace) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(2, UpdateAction::Change, 99, 25)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 25}, {98, 30}}));
}

TEST(MbpBookChange, PriceMismatchIsDesyncNotSilentCorrection) {
  auto book = three_bids();
  // The venue thinks level 2 is 98. We think it is 99. One of us has lost a
  // packet, and quietly adopting the venue's price would hide that forever.
  EXPECT_EQ(book.apply(bid(2, UpdateAction::Change, 98, 25)), ApplyResult::Desync);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}, {98, 30}}));
}

TEST(MbpBookChange, PastTheEndIsDesync) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(4, UpdateAction::Change, 97, 40)), ApplyResult::Desync);
}

// ─── Delete ──────────────────────────────────────────────────────────

TEST(MbpBookDelete, RemovesAndPullsUp) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(1, UpdateAction::Delete, 100, 10)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{99, 20}, {98, 30}}));
}

TEST(MbpBookDelete, RemovesTheBottom) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(3, UpdateAction::Delete, 98, 30)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}}));
}

TEST(MbpBookDelete, PastTheEndIsDesync) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(4, UpdateAction::Delete, 97, 0)), ApplyResult::Desync);
}

TEST(MbpBookDelete, ThruClearsTopN) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(2, UpdateAction::DeleteThru, 0, 0)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{98, 30}}));
}

TEST(MbpBookDelete, ThruBeyondDepthEmptiesTheSide) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(5, UpdateAction::DeleteThru, 0, 0)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {}));
}

TEST(MbpBookDelete, ThruPastInstrumentDepthStillEmptiesTheSide) {
  MbpBook book;
  book.set_depth(3);
  ASSERT_EQ(book.apply(bid(1, UpdateAction::New, 100, 10)), ApplyResult::Applied);
  ASSERT_EQ(book.apply(bid(2, UpdateAction::New, 99, 20)), ApplyResult::Applied);

  // "Delete levels 1 through 7" on a 3-deep instrument means everything goes.
  // Treating this as an out-of-depth clamp would leave a stale book claiming to
  // be healthy — the exact failure this action exists to prevent.
  EXPECT_EQ(book.apply(bid(7, UpdateAction::DeleteThru, 0, 0)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {}));
}

TEST(MbpBookDelete, FromClearsTailInclusive) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(2, UpdateAction::DeleteFrom, 0, 0)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}}));
}

TEST(MbpBookDelete, FromPastTheEndIsANoOp) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(9, UpdateAction::DeleteFrom, 0, 0)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}, {98, 30}}));
}

// ─── Overlay ─────────────────────────────────────────────────────────

TEST(MbpBookOverlay, ReplacesInPlaceIncludingPrice) {
  auto book = three_bids();
  // Unlike Change, Overlay is allowed to move the price at a level.
  EXPECT_EQ(book.apply(bid(2, UpdateAction::Overlay, 98, 99)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {98, 99}, {98, 30}}));
}

TEST(MbpBookOverlay, ExtendsByOne) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(4, UpdateAction::Overlay, 97, 40)), ApplyResult::Applied);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}, {98, 30}, {97, 40}}));
}

// ─── Addressing faults ───────────────────────────────────────────────

TEST(MbpBookAddressing, LevelZeroIsDesync) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(0, UpdateAction::New, 100, 10)), ApplyResult::Desync);
}

TEST(MbpBookAddressing, PastInstrumentDepthIsClampedNotLost) {
  MbpBook book;
  book.set_depth(2);
  ASSERT_EQ(book.apply(bid(1, UpdateAction::New, 100, 10)), ApplyResult::Applied);
  ASSERT_EQ(book.apply(bid(2, UpdateAction::New, 99, 20)), ApplyResult::Applied);
  // Level 3 on a 2-deep instrument is the venue reporting past what it publishes.
  EXPECT_EQ(book.apply(bid(3, UpdateAction::New, 98, 30)), ApplyResult::Clamped);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}}));
}

TEST(MbpBookAddressing, UnknownActionIsDesync) {
  auto book = three_bids();
  EXPECT_EQ(book.apply(bid(1, UpdateAction::Unknown, 100, 10)), ApplyResult::Desync);
}

TEST(MbpBookAddressing, DepthZeroMeansMaximum) {
  MbpBook book;
  book.set_depth(0);
  EXPECT_EQ(book.depth(), kMaxDepth);
}

TEST(MbpBookAddressing, DepthIsCappedAtMax) {
  MbpBook book;
  book.set_depth(200);
  EXPECT_EQ(book.depth(), kMaxDepth);
}

TEST(MbpBookAddressing, ShrinkingDepthTruncatesExistingLevels) {
  auto book = three_bids();
  book.set_depth(2);
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}}));
}

// ─── Sides are independent ───────────────────────────────────────────

TEST(MbpBookSides, BidAndAskDoNotInterfere) {
  MbpBook book;
  book.set_depth(5);
  ASSERT_EQ(book.apply(bid(1, UpdateAction::New, 100, 10)), ApplyResult::Applied);
  ASSERT_EQ(book.apply(ask(1, UpdateAction::New, 101, 11)), ApplyResult::Applied);

  ASSERT_EQ(book.bids().size(), 1U);
  ASSERT_EQ(book.asks().size(), 1U);
  EXPECT_EQ(book.best_bid()->px.raw, 100);
  EXPECT_EQ(book.best_ask()->px.raw, 101);
  EXPECT_FALSE(book.crossed());
}

TEST(MbpBookSides, CrossedIsDetected) {
  MbpBook book;
  book.set_depth(5);
  ASSERT_EQ(book.apply(bid(1, UpdateAction::New, 101, 10)), ApplyResult::Applied);
  ASSERT_EQ(book.apply(ask(1, UpdateAction::New, 101, 11)), ApplyResult::Applied);
  // Locked counts as crossed: bid >= ask.
  EXPECT_TRUE(book.crossed());
}

TEST(MbpBookSides, EmptySideIsNeverCrossed) {
  MbpBook book;
  ASSERT_EQ(book.apply(bid(1, UpdateAction::New, 100, 10)), ApplyResult::Applied);
  EXPECT_FALSE(book.crossed());
  EXPECT_EQ(book.best_ask(), nullptr);
}

TEST(MbpBookSides, TradesAreNotBookState) {
  MbpBook book;
  auto    trade = bid(1, UpdateAction::New, 100, 10);
  trade.type    = EntryType::Trade;
  EXPECT_EQ(book.apply(trade), ApplyResult::Applied);
  EXPECT_TRUE(book.empty());
}

// ─── install (snapshot path) ─────────────────────────────────────────

TEST(MbpBookInstall, PlacesLevelsAbsolutely) {
  MbpBook book;
  book.set_depth(5);
  book.install(Side::Bid, 1, Level{Mantissa{100}, 10, 1});
  book.install(Side::Bid, 2, Level{Mantissa{99}, 20, 2});
  EXPECT_TRUE(BidsAre(book, {{100, 10}, {99, 20}}));
}

TEST(MbpBookInstall, IgnoresOutOfRangeLevels) {
  MbpBook book;
  book.set_depth(2);
  book.install(Side::Bid, 0, Level{Mantissa{100}, 10, 1});
  book.install(Side::Bid, 3, Level{Mantissa{98}, 30, 1});
  EXPECT_TRUE(BidsAre(book, {}));
}

TEST(MbpBookInstall, ClearResetsBothSides) {
  auto book = three_bids();
  ASSERT_EQ(book.apply(ask(1, UpdateAction::New, 101, 11)), ApplyResult::Applied);
  book.clear();
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.bids().size(), 0U);
  EXPECT_EQ(book.asks().size(), 0U);
}

TEST(MbpBookInstall, EqualityComparesContentsNotCapacity) {
  auto a = three_bids();
  auto b = three_bids();
  EXPECT_TRUE(a == b);
  ASSERT_EQ(b.apply(bid(2, UpdateAction::Change, 99, 21)), ApplyResult::Applied);
  EXPECT_FALSE(a == b);
}

// ─── InstrumentBook routing ──────────────────────────────────────────

TEST(InstrumentBookRouting, ImpliedAndOutrightAreSeparateBooks) {
  InstrumentBook ib;
  ib.set_depth(5);

  auto outright = bid(1, UpdateAction::New, 100, 10);
  auto implied  = bid(1, UpdateAction::New, 100, 10);
  implied.type  = EntryType::ImpliedBid;

  ASSERT_EQ(ib.book_for(outright.type).apply(outright), ApplyResult::Applied);
  EXPECT_EQ(ib.outright.bids().size(), 1U);
  EXPECT_EQ(ib.implied.bids().size(), 0U);

  ASSERT_EQ(ib.book_for(implied.type).apply(implied), ApplyResult::Applied);
  EXPECT_EQ(ib.implied.bids().size(), 1U);
}

TEST(InstrumentBookRouting, ClearResetsRptSeqPriming) {
  InstrumentBook ib;
  ib.last_rpt_seq   = 42;
  ib.rpt_seq_primed = true;
  ib.clear();
  EXPECT_EQ(ib.last_rpt_seq, 0U);
  EXPECT_FALSE(ib.rpt_seq_primed);
}

// ─── Tick conversion ─────────────────────────────────────────────────

InstrumentDefinition def_with(std::int64_t increment, std::uint8_t sub_tick = 1,
                              SecurityId id = 7) {
  InstrumentDefinition def{};
  def.security            = id;
  def.min_price_increment = Mantissa{increment};
  def.sub_tick_divisor    = sub_tick;
  def.market_depth        = 10;
  return def;
}

TEST(TickConversion, ExactMultipleConverts) {
  const auto def = def_with(25'000'000);
  const auto px  = InstrumentStore::to_ticks(def, Mantissa{100'000'000});
  ASSERT_TRUE(px.has_value());
  EXPECT_EQ(px->ticks, 4);
}

TEST(TickConversion, SubTickPriceIsRejectedNotTruncated) {
  const auto def = def_with(25'000'000);
  // 4.5 ticks. Integer division would silently make this 4 and the price would
  // be wrong by half a tick for the rest of the session.
  const auto px = InstrumentStore::to_ticks(def, Mantissa{112'500'000});
  ASSERT_FALSE(px.has_value());
  EXPECT_EQ(px.error(), ConversionError::SubTickPrice);
}

TEST(TickConversion, SubTickDivisorMakesHalfTicksAddressable) {
  const auto def = def_with(25'000'000, 2);
  const auto px  = InstrumentStore::to_ticks(def, Mantissa{112'500'000});
  ASSERT_TRUE(px.has_value());
  EXPECT_EQ(px->ticks, 9);  // 112.5M / 12.5M
}

TEST(TickConversion, NegativePricesConvert) {
  const auto def = def_with(25'000'000);
  const auto px  = InstrumentStore::to_ticks(def, Mantissa{-100'000'000});
  ASSERT_TRUE(px.has_value());
  EXPECT_EQ(px->ticks, -4);
}

TEST(TickConversion, ZeroIncrementIsRejected) {
  const auto def = def_with(0);
  const auto px  = InstrumentStore::to_ticks(def, Mantissa{100});
  ASSERT_FALSE(px.has_value());
  EXPECT_EQ(px.error(), ConversionError::ZeroIncrement);
}

TEST(TickConversion, IndivisibleSubTickIsRejected) {
  const auto def = def_with(10, 3);  // 10 / 3 is not an integer
  const auto px  = InstrumentStore::to_ticks(def, Mantissa{30});
  ASSERT_FALSE(px.has_value());
  EXPECT_EQ(px.error(), ConversionError::BadSubTick);
}

TEST(TickConversion, RoundTripsThroughMantissa) {
  const auto def = def_with(25'000'000, 4);
  for (std::int64_t ticks = -20; ticks <= 20; ++ticks) {
    const auto mantissa = InstrumentStore::to_mantissa(def, Price{ticks});
    ASSERT_TRUE(mantissa.has_value());
    const auto back = InstrumentStore::to_ticks(def, *mantissa);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->ticks, ticks);
  }
}

// ─── InstrumentStore ─────────────────────────────────────────────────

TEST(InstrumentStoreTest, UpsertThenFind) {
  InstrumentStore store{16};
  ASSERT_TRUE(store.upsert(def_with(25'000'000, 1, 100)));
  const auto* def = store.find(100);
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(def->min_price_increment.raw, 25'000'000);
  EXPECT_EQ(store.size(), 1U);
}

TEST(InstrumentStoreTest, UpsertReplacesWithoutGrowing) {
  InstrumentStore store{16};
  ASSERT_TRUE(store.upsert(def_with(25'000'000, 1, 100)));
  ASSERT_TRUE(store.upsert(def_with(50'000'000, 1, 100)));
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.find(100)->min_price_increment.raw, 50'000'000);
}

TEST(InstrumentStoreTest, MissingInstrumentIsNullNotUndefined) {
  InstrumentStore store{16};
  EXPECT_EQ(store.find(999), nullptr);
  EXPECT_FALSE(store.contains(999));

  const auto px = store.to_ticks(999, Mantissa{100});
  ASSERT_FALSE(px.has_value());
  EXPECT_EQ(px.error(), ConversionError::NoInstrument);
}

TEST(InstrumentStoreTest, HoldsManyDistinctSecuritiesWithoutCollision) {
  InstrumentStore store{1024};
  for (SecurityId id = 1; id <= 500; ++id) {
    ASSERT_TRUE(store.upsert(def_with(25'000'000 + id, 1, id))) << "id " << id;
  }
  EXPECT_EQ(store.size(), 500U);
  for (SecurityId id = 1; id <= 500; ++id) {
    const auto* def = store.find(id);
    ASSERT_NE(def, nullptr) << "id " << id;
    EXPECT_EQ(def->min_price_increment.raw, 25'000'000 + id);
  }
}

TEST(InstrumentStoreTest, RefusesToOverfillRatherThanRehash) {
  // Capacity 16 → 32 slots → refuses past a load factor of 1/2.
  InstrumentStore store{16};
  std::size_t     accepted = 0;
  for (SecurityId id = 1; id <= 100; ++id) {
    if (store.upsert(def_with(25'000'000, 1, id))) ++accepted;
  }
  EXPECT_EQ(accepted, store.size());
  EXPECT_LE(store.size() * 2, store.capacity());
  // Everything it did accept must still be findable.
  std::size_t found = 0;
  for (SecurityId id = 1; id <= 100; ++id) {
    if (store.contains(id)) ++found;
  }
  EXPECT_EQ(found, accepted);
}

TEST(InstrumentStoreTest, ClearEmptiesButKeepsCapacity) {
  InstrumentStore store{16};
  ASSERT_TRUE(store.upsert(def_with(25'000'000, 1, 100)));
  const auto capacity = store.capacity();
  store.clear();
  EXPECT_EQ(store.size(), 0U);
  EXPECT_EQ(store.capacity(), capacity);
  EXPECT_FALSE(store.contains(100));
}

TEST(InstrumentStoreTest, SymbolViewStopsAtPaddingAndNul) {
  InstrumentDefinition def{};
  const std::string_view name = "ESZ5";
  for (std::size_t i = 0; i < name.size(); ++i) def.symbol[i] = name[i];
  for (std::size_t i = name.size(); i < kMaxSymbolLen; ++i) def.symbol[i] = ' ';
  EXPECT_EQ(def.symbol_view(), "ESZ5");
}

}  // namespace
}  // namespace rfeed
