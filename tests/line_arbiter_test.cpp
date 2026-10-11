#include "rfeed/line_arbiter.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "ref_venue.hpp"

namespace {

using namespace rfeed;
using rfeed::venue_ref::RefVenue;

// Small window so the blown-window path is reachable without 256 packets.
using Arbiter = LineArbiter<RefVenue, 8>;

// Each packet's payload is its own sequence number, so a test can assert on
// content as well as ordering and catch a ring slot handing back the wrong body.
struct Feeder {
  std::array<std::byte, 4> buf{};

  std::span<const std::byte> payload_for(SeqNum seq) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
      buf[i] = static_cast<std::byte>(static_cast<std::uint8_t>((seq >> (8U * i)) & 0xFFU));
    }
    return buf;
  }
};

SeqNum seq_of(std::span<const std::byte> payload) {
  SeqNum v = 0;
  for (std::size_t i = 0; i < payload.size() && i < 4; ++i) {
    v |= static_cast<SeqNum>(std::to_integer<std::uint8_t>(payload[i])) << (8U * i);
  }
  return v;
}

std::vector<SeqNum> drain(Arbiter& arb) {
  std::vector<SeqNum> out;
  while (const auto p = arb.next()) {
    EXPECT_EQ(seq_of(p->bytes), p->seq) << "ring slot returned another packet's body";
    out.push_back(p->seq);
  }
  return out;
}

TEST(LineArbiter, JoinsMidStreamAtTheFirstPacketSeen) {
  Arbiter arb;
  Feeder  f;

  // A live feed is never joined at sequence 1.
  arb.accept(50'000, f.payload_for(50'000), 1, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{50'000}));
  EXPECT_EQ(arb.expected(), 50'001U);
}

TEST(LineArbiter, InOrderPacketsPassStraightThrough) {
  Arbiter arb;
  Feeder  f;

  std::vector<SeqNum> seen;
  for (SeqNum s = 1; s <= 5; ++s) {
    arb.accept(s, f.payload_for(s), static_cast<Timestamp>(s), LineId::A);
    for (const auto v : drain(arb)) seen.push_back(v);
  }

  EXPECT_EQ(seen, (std::vector<SeqNum>{1, 2, 3, 4, 5}));
  EXPECT_EQ(arb.stats().out_of_order, 0U);
  EXPECT_EQ(arb.buffered(), 0U);
}

TEST(LineArbiter, DuplicateFromTheSlowerLineIsDropped) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 10, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{1}));

  arb.accept(1, f.payload_for(1), 11, LineId::B);  // B arrives late with the same packet
  EXPECT_TRUE(drain(arb).empty());

  EXPECT_EQ(arb.stats().duplicates[static_cast<std::size_t>(LineId::B)], 1U);
  EXPECT_EQ(arb.stats().first_arrivals[static_cast<std::size_t>(LineId::A)], 1U);
  EXPECT_EQ(arb.stats().emitted, 1U);
}

TEST(LineArbiter, ADropOnOneLineOnlyIsInvisible) {
  // The single most important arbiter property: if A loses packets that B
  // carries, no gap may ever be declared and the output must be complete.
  Arbiter arb;
  Feeder  f;

  std::vector<SeqNum> seen;
  for (SeqNum s = 1; s <= 20; ++s) {
    const bool a_drops = (s % 3 == 0);
    if (!a_drops) {
      arb.accept(s, f.payload_for(s), static_cast<Timestamp>(s * 100), LineId::A);
      for (const auto v : drain(arb)) seen.push_back(v);
    }
    arb.accept(s, f.payload_for(s), static_cast<Timestamp>(s * 100 + 10), LineId::B);
    for (const auto v : drain(arb)) seen.push_back(v);
  }

  std::vector<SeqNum> expected(20);
  for (std::size_t i = 0; i < 20; ++i) expected[i] = static_cast<SeqNum>(i + 1);

  EXPECT_EQ(seen, expected);
  EXPECT_FALSE(arb.gap_matured(1'000'000'000)) << "a one-line drop must never declare a gap";
  EXPECT_EQ(arb.stats().window_blown, 0U);
  EXPECT_EQ(arb.buffered(), 0U);
  // B won every race for the packets A dropped.
  EXPECT_EQ(arb.stats().first_arrivals[static_cast<std::size_t>(LineId::B)], 6U);
}

TEST(LineArbiter, ReorderWithinTheWindowIsAbsorbedWithoutAGap) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 100, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{1}));

  // 3 and 4 arrive before 2.
  arb.accept(3, f.payload_for(3), 200, LineId::A);
  EXPECT_TRUE(drain(arb).empty());
  arb.accept(4, f.payload_for(4), 201, LineId::A);
  EXPECT_TRUE(drain(arb).empty());
  EXPECT_EQ(arb.buffered(), 2U);

  arb.accept(2, f.payload_for(2), 202, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{2, 3, 4})) << "must emit in sequence order";
  EXPECT_EQ(arb.buffered(), 0U);
  EXPECT_FALSE(arb.gap_matured(1'000'000'000));
}

TEST(LineArbiter, AHoleIsNotAGapUntilTheDeadlineExpires) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 0, LineId::A);
  drain(arb);
  arb.accept(3, f.payload_for(3), 1000, LineId::A);  // 2 is missing

  const auto deadline = static_cast<Timestamp>(RefVenue::kGapDeadline.count());
  EXPECT_FALSE(arb.gap_matured(1000)) << "the other line usually fills this in microseconds";
  EXPECT_FALSE(arb.gap_matured(1000 + deadline - 1));
  EXPECT_TRUE(arb.gap_matured(1000 + deadline));

  const auto gap = arb.pending_gap();
  EXPECT_EQ(gap.begin, 2U);
  EXPECT_EQ(gap.end, 2U);
  EXPECT_EQ(gap.count(), 1U);
}

TEST(LineArbiter, DeadlineIsDisarmedWhenTheHoleFillsInTime) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 0, LineId::A);
  drain(arb);
  arb.accept(3, f.payload_for(3), 1000, LineId::A);
  arb.accept(2, f.payload_for(2), 1100, LineId::B);
  drain(arb);

  EXPECT_FALSE(arb.gap_matured(999'999'999));
  EXPECT_EQ(arb.buffered(), 0U);
}

TEST(LineArbiter, BlowingTheWindowIsAnImmediateHardGap) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 0, LineId::A);
  drain(arb);

  // Window is 8; jumping to 1000 is far beyond anything reorder can explain.
  arb.accept(1000, f.payload_for(1000), 10, LineId::A);

  EXPECT_TRUE(arb.hard_gap());
  EXPECT_TRUE(arb.gap_matured(10)) << "no point waiting on the other line";
  EXPECT_EQ(arb.stats().window_blown, 1U);
  EXPECT_TRUE(drain(arb).empty()) << "the far-ahead packet must not be emitted out of order";

  const auto gap = arb.pending_gap();
  EXPECT_EQ(gap.begin, 2U);
  EXPECT_EQ(gap.count(), 0U) << "unbounded gap: nothing buffered to bound it";
}

TEST(LineArbiter, ResyncJumpsForwardAndDropsStaleBufferedPackets) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 0, LineId::A);
  drain(arb);
  arb.accept(3, f.payload_for(3), 10, LineId::A);
  arb.accept(4, f.payload_for(4), 11, LineId::A);
  EXPECT_EQ(arb.buffered(), 2U);

  // Recovery delivered everything through 3.
  arb.resync_to(4);
  EXPECT_EQ(arb.expected(), 4U);
  EXPECT_EQ(arb.buffered(), 1U) << "packet 3 is stale; 4 is still wanted";
  EXPECT_FALSE(arb.hard_gap());
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{4}));
}

TEST(LineArbiter, ResetRestartsTheSequenceSpace) {
  Arbiter arb;
  Feeder  f;

  arb.accept(500, f.payload_for(500), 0, LineId::A);
  drain(arb);
  arb.accept(502, f.payload_for(502), 10, LineId::A);
  ASSERT_EQ(arb.buffered(), 1U);

  arb.reset();
  EXPECT_EQ(arb.expected(), 1U);
  EXPECT_EQ(arb.buffered(), 0U);
  EXPECT_FALSE(arb.hard_gap());

  arb.accept(1, f.payload_for(1), 20, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{1}));
}

TEST(LineArbiter, SequenceNumberWrapIsHandled) {
  Arbiter arb;
  Feeder  f;

  constexpr SeqNum kNearMax = 0xFFFFFFFEU;
  arb.accept(kNearMax, f.payload_for(kNearMax), 0, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{kNearMax}));

  arb.accept(0xFFFFFFFFU, f.payload_for(0xFFFFFFFFU), 1, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{0xFFFFFFFFU}));

  arb.accept(0, f.payload_for(0), 2, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{0})) << "wrap must not read as a 4-billion gap";
  EXPECT_FALSE(arb.hard_gap());

  arb.accept(1, f.payload_for(1), 3, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{1}));
}

TEST(LineArbiter, StaleDuplicateAfterWrapIsStillADuplicate) {
  Arbiter arb;
  Feeder  f;

  arb.accept(0xFFFFFFFFU, f.payload_for(0xFFFFFFFFU), 0, LineId::A);
  drain(arb);
  arb.accept(0, f.payload_for(0), 1, LineId::A);
  drain(arb);

  arb.accept(0xFFFFFFFFU, f.payload_for(0xFFFFFFFFU), 2, LineId::B);
  EXPECT_TRUE(drain(arb).empty());
  EXPECT_EQ(arb.stats().duplicates[static_cast<std::size_t>(LineId::B)], 1U);
}

TEST(LineArbiter, SilentLineIsDetectedWithoutAffectingTheBook) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 1'000'000, LineId::A);
  drain(arb);

  constexpr Timestamp kMaxSilence = 3'000'000'000;  // 3s
  EXPECT_TRUE(arb.line_silent(LineId::B, 1'000'000, kMaxSilence)) << "B never spoke";
  EXPECT_FALSE(arb.line_silent(LineId::A, 1'000'000, kMaxSilence));
  EXPECT_TRUE(arb.line_silent(LineId::A, 5'000'000'000, kMaxSilence));

  // A silent line is an alert, never a gap.
  EXPECT_FALSE(arb.gap_matured(5'000'000'000));
}

TEST(LineArbiter, FullWindowOfReorderStillDrainsInOrder) {
  Arbiter arb;
  Feeder  f;

  arb.accept(1, f.payload_for(1), 0, LineId::A);
  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{1}));

  // Fill the window backwards: 8,7,6,5,4,3,2.
  for (SeqNum s = 8; s >= 2; --s) {
    arb.accept(s, f.payload_for(s), 10, LineId::A);
    if (s > 2) {
      EXPECT_TRUE(drain(arb).empty());
    }
  }

  EXPECT_EQ(drain(arb), (std::vector<SeqNum>{2, 3, 4, 5, 6, 7, 8}));
  EXPECT_EQ(arb.buffered(), 0U);
}

}  // namespace
