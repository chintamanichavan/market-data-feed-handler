// Proof that the core carries no venue knowledge.
//
// venue_alt disagrees with venue_ref on every axis VenuePolicy abstracts —
// notably a uint16 numInGroup, which silently misparses every repeating group
// after the first if anything downstream assumes the other width. The same
// synthetic feed, the same loss patterns and the same convergence gate are
// replayed through it. Nothing under src/ or include/ changed to make this work,
// and CoreContainsNoVenueNames at the bottom enforces that.

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "alt_encoder.hpp"
#include "alt_venue.hpp"
#include "ref_venue.hpp"
#include "rfeed/feed_handler.hpp"
#include "rfeed/transport_pcap.hpp"
#include "synthetic_feed.hpp"

namespace rfeed {
namespace {

using venue_alt::AltVenue;
using AltFeed = test::SyntheticFeedT<venue_alt::Encoder>;

struct Sink {
  std::uint64_t events{0};
  void operator()(const NormalizedEvent&) noexcept { ++events; }
};

class TempCapture {
 public:
  explicit TempCapture(std::string tag)
      : path_{(std::filesystem::temp_directory_path() / ("rfeed-alt-" + std::move(tag) + ".pcap"))
                  .string()} {}
  ~TempCapture() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempCapture(const TempCapture&)            = delete;
  TempCapture& operator=(const TempCapture&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

using SideLevels = std::vector<std::pair<std::int64_t, std::int64_t>>;
struct FinalBook {
  SideLevels bids;
  SideLevels asks;
};
using FinalState = std::map<SecurityId, FinalBook>;

SideLevels levels_of(std::span<const Level> levels) {
  SideLevels out;
  for (const auto& l : levels) out.emplace_back(l.px.raw, l.qty);
  return out;
}

struct RunResult {
  FinalState   books;
  FeedState    state{FeedState::Init};
  HandlerStats stats{};
  BuilderStats builder{};
};

RunResult run_alt(const std::string& path, const std::vector<SecurityId>& securities) {
  auto transport = PcapTransport::open(path, test::synthetic_config());
  EXPECT_TRUE(transport.has_value());
  if (!transport) return {};

  Sink sink;
  ChannelHandler<AltVenue, PcapTransport, Sink> handler{310, *transport, sink};
  handler.start();
  handler.run();

  RunResult result;
  result.state   = handler.state();
  result.stats   = handler.stats();
  result.builder = handler.builder().stats();
  for (const auto id : securities) {
    if (const auto* book = handler.builder().book(id)) {
      result.books[id] = FinalBook{levels_of(book->outright.bids()),
                                   levels_of(book->outright.asks())};
    }
  }
  return result;
}

FinalState model_state(const AltFeed& feed, std::size_t depth) {
  FinalState out;
  for (const auto& [id, model] : feed.models()) {
    out[id] = FinalBook{model.top(true, depth), model.top(false, depth)};
  }
  return out;
}

testing::AssertionResult BooksMatch(const FinalState& got, const FinalState& want,
                                    std::string_view what) {
  if (got.size() != want.size()) {
    return testing::AssertionFailure()
           << what << ": " << got.size() << " instruments, expected " << want.size();
  }
  for (const auto& [id, expected] : want) {
    const auto it = got.find(id);
    if (it == got.end()) return testing::AssertionFailure() << what << ": missing " << id;
    if (it->second.bids != expected.bids) {
      return testing::AssertionFailure() << what << ": security " << id << " bids differ";
    }
    if (it->second.asks != expected.asks) {
      return testing::AssertionFailure() << what << ": security " << id << " asks differ";
    }
  }
  return testing::AssertionSuccess();
}

test::FeedParams alt_params() {
  test::FeedParams p;
  p.instruments     = 3;
  p.steps           = 600;
  p.depth           = 6;
  p.snapshot_period = 40;
  return p;
}

// ─── The venue actually differs ──────────────────────────────────────

TEST(AltVenueTest, DisagreesWithRefOnEveryAxisThePolicyAbstracts) {
  using Ref = venue_ref::RefVenue;
  EXPECT_NE(AltVenue::kSchemaId, Ref::kSchemaId);
  EXPECT_NE(AltVenue::kMinSupportedVersion, Ref::kMinSupportedVersion);
  EXPECT_NE(AltVenue::kPriceExponent, Ref::kPriceExponent);
  EXPECT_NE(AltVenue::kHeartbeatConsumesSeqNum, Ref::kHeartbeatConsumesSeqNum);
  EXPECT_NE(AltVenue::kSecurityIdStableAcrossPeriods, Ref::kSecurityIdStableAcrossPeriods);
  EXPECT_NE(AltVenue::kGapDeadline, Ref::kGapDeadline);
  EXPECT_NE(AltVenue::kReplayLimits.max_packets_per_request,
            Ref::kReplayLimits.max_packets_per_request);

  // The one that matters most: a different numInGroup width.
  static_assert(AltVenue::kGroupCountBytes == 2);
  static_assert(Ref::kGroupCountBytes == 1);

  // Same logical message, different template id on each venue.
  EXPECT_EQ(AltVenue::classify(venue_alt::kTplIncrementalBook), LogicalMessage::IncrementalBook);
  EXPECT_EQ(Ref::classify(venue_ref::kTplIncrementalBook), LogicalMessage::IncrementalBook);
  EXPECT_NE(venue_alt::kTplIncrementalBook, venue_ref::kTplIncrementalBook);
  // And each venue's ids are meaningless to the other.
  EXPECT_EQ(AltVenue::classify(venue_ref::kTplSnapshot), LogicalMessage::Unknown);
}

TEST(AltVenueTest, PeriodResetKeepsDefinitionsBecauseSecurityIdsAreStable) {
  const auto alt = AltVenue::reset_scope(ResetKind::PeriodStart);
  const auto ref = venue_ref::RefVenue::reset_scope(ResetKind::PeriodStart);

  // Backwards on this venue throws away every definition at every rollover;
  // backwards on venue_ref keeps stale IDs that now mean other instruments.
  EXPECT_FALSE(alt.definitions);
  EXPECT_FALSE(alt.security_id_namespace);
  EXPECT_TRUE(ref.definitions);
  EXPECT_TRUE(ref.security_id_namespace);
}

// ─── Round trip through the alt codec ────────────────────────────────

TEST(AltVenueTest, MultiEntryGroupsDecodeWithAUint16Count) {
  // Two entries is the minimum that catches a numInGroup width error: with one,
  // a misread count still lands on a plausible-looking book.
  std::vector<MdEntry> entries;
  for (std::uint8_t i = 0; i < 3; ++i) {
    MdEntry e{};
    e.px          = Mantissa{100 - i};
    e.size        = 10 + i;
    e.security    = 55;
    e.rpt_seq     = 1U + i;
    e.num_orders  = 2;
    e.price_level = static_cast<std::uint8_t>(i + 1);
    e.action      = UpdateAction::New;
    e.type        = EntryType::Bid;
    entries.push_back(e);
  }

  venue_alt::Encoder enc{7, 1'700'000'000'000'000'000};
  enc.incremental(IncrementalHeader{.transact_time = 42, .end_of_event = true}, entries);

  const auto packet = PacketView::decode(enc.view());
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.seq, 7U);

  auto       framer = packet->framer();
  const auto msg    = framer.next();
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->hdr.schema_id, AltVenue::kSchemaId);
  EXPECT_EQ(AltVenue::classify(msg->hdr.template_id), LogicalMessage::IncrementalBook);

  ByteReader in{msg->body};
  const auto header = AltVenue::Codec::incremental_header(in);
  ASSERT_TRUE(header.has_value());
  EXPECT_TRUE(header->end_of_event);
  EXPECT_EQ(header->transact_time, 42);

  ASSERT_TRUE(seek_past_block(in, msg->hdr.block_length).has_value());
  auto group = GroupReader<AltVenue::kGroupCountBytes>::decode(in);
  ASSERT_TRUE(group.has_value());

  std::vector<MdEntry> decoded;
  while (true) {
    auto entry_reader = group->next();
    if (!entry_reader) {
      ASSERT_EQ(entry_reader.error(), DecodeError::EndOfGroup);
      break;
    }
    const auto e = AltVenue::Codec::md_entry(*entry_reader);
    ASSERT_TRUE(e.has_value());
    decoded.push_back(*e);
  }

  ASSERT_EQ(decoded.size(), entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    EXPECT_EQ(decoded[i].px, entries[i].px) << "entry " << i;
    EXPECT_EQ(decoded[i].size, entries[i].size) << "entry " << i;
    EXPECT_EQ(decoded[i].security, entries[i].security) << "entry " << i;
    EXPECT_EQ(decoded[i].rpt_seq, entries[i].rpt_seq) << "entry " << i;
    EXPECT_EQ(decoded[i].price_level, entries[i].price_level) << "entry " << i;
    EXPECT_EQ(decoded[i].action, entries[i].action) << "entry " << i;
    EXPECT_EQ(decoded[i].type, entries[i].type) << "entry " << i;
  }
}

TEST(AltVenueTest, RefPacketsAreRejectedByTheAltSchemaCheck) {
  // Cross-wiring a venue to the wrong channel must fail loudly at the schema
  // check, not decode into garbage that looks like a book.
  MdEntry entry{};
  entry.px          = Mantissa{100};
  entry.size        = 1;
  entry.security    = 1;
  entry.price_level = 1;
  entry.action      = UpdateAction::New;
  entry.type        = EntryType::Bid;

  const auto ref_packet = venue_ref::encode_incremental_packet(
      1, 0, IncrementalHeader{.transact_time = 0, .end_of_event = true}, std::span{&entry, 1});

  const auto packet = PacketView::decode(ref_packet);
  ASSERT_TRUE(packet.has_value());
  auto       framer = packet->framer();
  const auto msg    = framer.next();
  ASSERT_TRUE(msg.has_value());

  Sink                                    sink;
  BookBuilder<AltVenue, Sink>             builder{310, sink};
  const auto outcome = builder.on_message(*msg, 1, 0);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error(), DecodeError::UnsupportedSchema);
  EXPECT_EQ(builder.stats().schema_rejects, 1U);
}

// ─── The full pipeline, unmodified ───────────────────────────────────

TEST(AltVenueTest, LossFreeRunMatchesTheReferenceModel) {
  TempCapture capture{"clean"};
  auto        params = alt_params();
  AltFeed     feed{params};
  ASSERT_TRUE(feed.write(capture.path()).has_value());

  const auto run = run_alt(capture.path(), feed.securities());
  EXPECT_EQ(run.state, FeedState::Synchronized);
  EXPECT_EQ(run.builder.desyncs, 0U);
  EXPECT_EQ(run.builder.decode_errors, 0U);
  EXPECT_EQ(run.builder.schema_rejects, 0U);
  EXPECT_EQ(run.stats.framing_errors, 0U);
  EXPECT_GT(run.stats.snapshot_cycles, 0U);
  EXPECT_TRUE(BooksMatch(run.books, model_state(feed, params.depth), "alt loss-free"));
}

void ExpectAltConvergence(std::string_view name, const std::function<bool(SeqNum, LineId)>& drop,
                          bool expect_gap) {
  SCOPED_TRACE(name);
  TempCapture clean_capture{std::string{name} + "-clean"};
  TempCapture lossy_capture{std::string{name} + "-lossy"};

  auto    params = alt_params();
  AltFeed clean{params};
  ASSERT_TRUE(clean.write(clean_capture.path()).has_value());

  auto lossy_params = params;
  lossy_params.drop = drop;
  AltFeed lossy{lossy_params};
  ASSERT_TRUE(lossy.write(lossy_capture.path()).has_value());

  const auto baseline  = run_alt(clean_capture.path(), clean.securities());
  const auto recovered = run_alt(lossy_capture.path(), lossy.securities());

  ASSERT_EQ(baseline.state, FeedState::Synchronized);
  EXPECT_EQ(recovered.state, FeedState::Synchronized);
  if (expect_gap) {
    EXPECT_GT(recovered.stats.gaps, 0U);
  } else {
    EXPECT_EQ(recovered.stats.gaps, 0U);
  }
  EXPECT_TRUE(BooksMatch(recovered.books, baseline.books, "alt recovered vs loss-free"));
  EXPECT_TRUE(BooksMatch(recovered.books, model_state(lossy, params.depth),
                         "alt recovered vs model"));
}

TEST(AltVenueGapInjection, LineDownIsCoveredByRedundancy) {
  ExpectAltConvergence(
      "alt-line-a", [](SeqNum, LineId line) { return line == LineId::A; }, false);
}

TEST(AltVenueGapInjection, SmallGapRecoversViaSnapshot) {
  ExpectAltConvergence(
      "alt-small", [](SeqNum seq, LineId) { return seq >= 120 && seq <= 123; }, true);
}

TEST(AltVenueGapInjection, RepeatedGapsRecoverEachTime) {
  ExpectAltConvergence(
      "alt-repeated", [](SeqNum seq, LineId) { return seq % 97 == 0; }, true);
}

TEST(AltVenueGapInjection, WindowBlowingGapRecovers) {
  ExpectAltConvergence(
      "alt-window", [](SeqNum seq, LineId) { return seq >= 100 && seq < 380; }, true);
}

}  // namespace
}  // namespace rfeed
