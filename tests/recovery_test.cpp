// Recovery: the FSM in isolation, and then the only test that really matters —
//
//   the final book after an injected gap must equal the final book from the
//   loss-free run, for every shape of loss.
//
// Everything else in this file exists to make that claim trustworthy. The books
// are also cross-checked against SyntheticFeed's price-keyed reference model,
// which is an independent data structure, so a level-indexing bug cannot hide by
// being made identically in both.

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ref_venue.hpp"
#include "rfeed/feed_handler.hpp"
#include "rfeed/packet_buffer.hpp"
#include "rfeed/recovery.hpp"
#include "rfeed/transport_pcap.hpp"
#include "synthetic_feed.hpp"

namespace rfeed {
namespace {

using venue_ref::RefVenue;

// ─── Fixtures ────────────────────────────────────────────────────────

struct CountingSink {
  std::uint64_t              books{0};
  std::uint64_t              trades{0};
  std::uint64_t              instruments{0};
  std::uint64_t              statuses{0};
  std::vector<FeedStateChange> transitions{};

  void operator()(const NormalizedEvent& event) noexcept {
    std::visit(
        [this](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, BookUpdate>) {
            ++books;
          } else if constexpr (std::is_same_v<T, TradeTick>) {
            ++trades;
          } else if constexpr (std::is_same_v<T, InstrumentUpdate>) {
            ++instruments;
          } else if constexpr (std::is_same_v<T, StatusChange>) {
            ++statuses;
          } else if constexpr (std::is_same_v<T, FeedStateChange>) {
            transitions.push_back(e);
          }
        },
        event);
  }
};
static_assert(EventSink<CountingSink>);

// Scratch pcap that cleans up after itself.
class TempCapture {
 public:
  explicit TempCapture(std::string tag)
      : path_{(std::filesystem::temp_directory_path() / ("rfeed-" + std::move(tag) + ".pcap"))
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

// The book state the handler ended up with, in a form that can be compared
// across runs and against the reference model.
using SideLevels = std::vector<std::pair<std::int64_t, std::int64_t>>;
struct FinalBook {
  SideLevels bids;
  SideLevels asks;
  auto operator<=>(const FinalBook&) const = default;
};
using FinalState = std::map<SecurityId, FinalBook>;

SideLevels levels_of(std::span<const Level> levels) {
  SideLevels out;
  out.reserve(levels.size());
  for (const auto& l : levels) out.emplace_back(l.px.raw, l.qty);
  return out;
}

struct RunResult {
  FinalState   books;
  FeedState    state{FeedState::Init};
  HandlerStats stats{};
  BuilderStats builder{};
  std::uint64_t replay_skipped{0};
};

// Runs one capture end to end and harvests the final book of every instrument.
RunResult run_capture(const std::string& path, const std::vector<SecurityId>& securities) {
  auto transport = PcapTransport::open(path, test::synthetic_config());
  EXPECT_TRUE(transport.has_value()) << "could not open " << path;
  if (!transport) return {};

  CountingSink sink;
  ChannelHandler<RefVenue, PcapTransport, CountingSink> handler{310, *transport, sink};
  handler.start();
  handler.run();

  RunResult result;
  result.state          = handler.state();
  result.stats          = handler.stats();
  result.builder        = handler.builder().stats();
  result.replay_skipped = handler.builder().stats().replay_skipped;

  for (const auto id : securities) {
    const auto* book = handler.builder().book(id);
    if (book == nullptr) continue;
    result.books[id] = FinalBook{levels_of(book->outright.bids()),
                                 levels_of(book->outright.asks())};
  }
  return result;
}

// The independent oracle: SyntheticFeed's price-keyed std::map.
FinalState model_state(const test::SyntheticFeed& feed, std::size_t depth) {
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
    if (it == got.end()) return testing::AssertionFailure() << what << ": missing security " << id;
    if (it->second.bids != expected.bids) {
      return testing::AssertionFailure()
             << what << ": security " << id << " bid side differs (" << it->second.bids.size()
             << " vs " << expected.bids.size() << " levels)";
    }
    if (it->second.asks != expected.asks) {
      return testing::AssertionFailure()
             << what << ": security " << id << " ask side differs (" << it->second.asks.size()
             << " vs " << expected.asks.size() << " levels)";
    }
  }
  return testing::AssertionSuccess();
}

test::FeedParams base_params() {
  test::FeedParams p;
  p.instruments     = 3;
  p.steps           = 600;
  p.depth           = 6;
  p.snapshot_period = 40;
  return p;
}

// ═════════════════════════════════════════════════════════════════════
// SnapshotCycle / DefinitionCycle
// ═════════════════════════════════════════════════════════════════════

SnapshotHeader snap_header(SecurityId id, std::uint32_t tot) {
  return SnapshotHeader{.last_msg_seq_num_processed = 0,
                        .tot_num_reports            = tot,
                        .security                   = id,
                        .rpt_seq                    = 0,
                        .transact_time              = 0,
                        .status                     = TradingStatus::Open};
}

TEST(SnapshotCycleTest, CompletesOnTotNumReports) {
  SnapshotCycle cycle;
  cycle.begin(0);
  EXPECT_FALSE(cycle.observe(snap_header(1, 3)));
  EXPECT_FALSE(cycle.observe(snap_header(2, 3)));
  EXPECT_TRUE(cycle.observe(snap_header(3, 3)));
  EXPECT_EQ(cycle.reports_seen(), 3U);
}

TEST(SnapshotCycleTest, CompletesOnWraparoundWhenTotIsUnreliable) {
  SnapshotCycle cycle;
  cycle.begin(0);
  // TotNumReports of 0 — some venues do not populate it. Joining mid-cycle, the
  // only honest signal left is the first instrument coming round again.
  EXPECT_FALSE(cycle.observe(snap_header(7, 0)));
  EXPECT_FALSE(cycle.observe(snap_header(8, 0)));
  EXPECT_FALSE(cycle.observe(snap_header(9, 0)));
  EXPECT_TRUE(cycle.observe(snap_header(7, 0)));
  EXPECT_TRUE(cycle.wrapped());
}

TEST(SnapshotCycleTest, JoinedMidCycleStillCompletes) {
  SnapshotCycle cycle;
  cycle.begin(0);
  // Joined at instrument 3 of 3. The loop is cyclic, so any TotNumReports
  // consecutive reports cover every instrument exactly once regardless of where
  // the join landed — which is precisely why counting reports is sound and
  // tracking distinct securities would be redundant.
  EXPECT_FALSE(cycle.observe(snap_header(3, 3)));
  EXPECT_FALSE(cycle.observe(snap_header(1, 3)));
  EXPECT_TRUE(cycle.observe(snap_header(2, 3)));
  EXPECT_FALSE(cycle.wrapped());
}

TEST(SnapshotCycleTest, InactiveCycleIgnoresReports) {
  SnapshotCycle cycle;
  EXPECT_FALSE(cycle.observe(snap_header(1, 1)));
  cycle.begin(0);
  cycle.end();
  EXPECT_FALSE(cycle.observe(snap_header(1, 1)));
}

TEST(SnapshotCycleTest, TimesOutOnlyWhileActive) {
  SnapshotCycle cycle;
  EXPECT_FALSE(cycle.timed_out(1'000'000, 1));
  cycle.begin(0);
  EXPECT_FALSE(cycle.timed_out(999, 1000));
  EXPECT_TRUE(cycle.timed_out(1000, 1000));
  cycle.end();
  EXPECT_FALSE(cycle.timed_out(1'000'000, 1000));
}

TEST(DefinitionCycleTest, CompletesOnTotNumReports) {
  DefinitionCycle cycle;
  cycle.begin(0);
  InstrumentDefinition def{};
  def.tot_num_reports = 2;
  def.security        = 1;
  EXPECT_FALSE(cycle.observe(def));
  def.security = 2;
  EXPECT_TRUE(cycle.observe(def));
}

// ═════════════════════════════════════════════════════════════════════
// RecoveryFsm transition matrix
// ═════════════════════════════════════════════════════════════════════

TEST(RecoveryFsmTest, StartsByAwaitingDefinitions) {
  RecoveryFsm<RefVenue> fsm;
  EXPECT_EQ(fsm.state(), FeedState::Init);
  const auto d = fsm.start(0);
  EXPECT_EQ(fsm.state(), FeedState::AwaitingDefinitions);
  // Nothing is published before we know what the instruments are.
  EXPECT_FALSE(d.publish);
  EXPECT_FALSE(d.buffer_incrementals);
}

TEST(RecoveryFsmTest, SkipsDefinitionsWhenTheVenueHasNoSuchChannel) {
  RecoveryConfig config;
  config.has_definition_channel = false;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
}

TEST(RecoveryFsmTest, WalksInitToSynchronized) {
  RecoveryFsm<RefVenue> fsm;
  fsm.start(0);
  const auto snap = fsm.on_definitions_complete(1);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_TRUE(snap.buffer_incrementals);

  const auto live = fsm.on_snapshot_cycle_complete(2);
  EXPECT_EQ(fsm.state(), FeedState::Synchronized);
  EXPECT_FALSE(live.buffer_incrementals);
  EXPECT_TRUE(live.publish);
  EXPECT_FALSE(fsm.degraded());
}

TEST(RecoveryFsmTest, GapWithoutReplayGoesToSnapshotSync) {
  RecoveryFsm<RefVenue> fsm;
  fsm.start(0);
  fsm.on_definitions_complete(1);
  fsm.on_snapshot_cycle_complete(2);

  const auto d = fsm.on_gap(GapRange{10, 12}, 3);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_TRUE(d.restart_snapshot);
  EXPECT_TRUE(d.clear_books);
  EXPECT_TRUE(d.buffer_incrementals);
  // Natural refresh runs alongside real recovery; books say Degraded, and the
  // feed never claims to be authoritative while this flag is set.
  EXPECT_TRUE(fsm.degraded());
}

TEST(RecoveryFsmTest, BoundedGapPrefersReplayWhenAvailable) {
  RecoveryConfig config;
  config.replay_available = true;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_definitions_complete(1);
  fsm.on_snapshot_cycle_complete(2);

  const auto d = fsm.on_gap(GapRange{10, 12}, 3);
  EXPECT_EQ(fsm.state(), FeedState::ReplaySync);
  EXPECT_TRUE(d.request_replay);

  fsm.on_replay_complete(4);
  EXPECT_EQ(fsm.state(), FeedState::Synchronized);
  EXPECT_FALSE(fsm.degraded());
}

TEST(RecoveryFsmTest, GapTooLargeForReplayFallsBackToSnapshot) {
  RecoveryConfig config;
  config.replay_available = true;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_definitions_complete(1);
  fsm.on_snapshot_cycle_complete(2);

  const SeqNum begin = 10;
  const SeqNum end   = begin + RefVenue::kReplayLimits.max_packets_per_request;  // one too many
  const auto   d     = fsm.on_gap(GapRange{begin, end}, 3);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_FALSE(d.request_replay);
  EXPECT_TRUE(d.restart_snapshot);
}

TEST(RecoveryFsmTest, UnboundedGapNeverAsksForReplay) {
  RecoveryConfig config;
  config.replay_available = true;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_definitions_complete(1);
  fsm.on_snapshot_cycle_complete(2);

  // end == begin - 1 is the arbiter's encoding for "we do not know how big".
  // Asking a venue to replay an unknown range is how you get rate-limited off
  // the feed.
  const auto d = fsm.on_gap(GapRange{10, 9}, 3);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_FALSE(d.request_replay);
}

TEST(RecoveryFsmTest, ReplayFailureFallsBackToSnapshotAndIsNotTerminal) {
  RecoveryConfig config;
  config.replay_available = true;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_definitions_complete(1);
  fsm.on_snapshot_cycle_complete(2);
  fsm.on_gap(GapRange{10, 12}, 3);
  ASSERT_EQ(fsm.state(), FeedState::ReplaySync);

  const auto d = fsm.on_replay_failed(4);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_TRUE(d.restart_snapshot);
  EXPECT_NE(fsm.state(), FeedState::Failed);
}

TEST(RecoveryFsmTest, WindowBlownRestartsTheJoin) {
  RecoveryFsm<RefVenue> fsm;
  fsm.start(0);
  fsm.on_definitions_complete(1);
  fsm.on_snapshot_cycle_complete(2);

  const auto d = fsm.on_window_blown(3);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_TRUE(d.restart_snapshot);
  EXPECT_TRUE(d.clear_books);
  EXPECT_TRUE(fsm.degraded());
}

TEST(RecoveryFsmTest, ChannelResetGoesBackToTheBeginning) {
  RecoveryFsm<RefVenue> fsm;
  fsm.start(0);
  fsm.on_definitions_complete(1);
  fsm.on_snapshot_cycle_complete(2);
  ASSERT_EQ(fsm.state(), FeedState::Synchronized);

  const auto d = fsm.on_channel_reset(3);
  EXPECT_EQ(fsm.state(), FeedState::AwaitingDefinitions);
  EXPECT_TRUE(d.clear_books);
}

TEST(RecoveryFsmTest, DefinitionTimeoutFailsLoudlyRatherThanHanging) {
  RecoveryConfig config;
  config.definition_cycle_timeout_ns = 1000;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_tick(999);
  EXPECT_EQ(fsm.state(), FeedState::AwaitingDefinitions);
  fsm.on_tick(1000);
  EXPECT_EQ(fsm.state(), FeedState::Failed);
}

TEST(RecoveryFsmTest, SnapshotRetriesThenFails) {
  RecoveryConfig config;
  config.snapshot_cycle_timeout_ns = 1000;
  config.max_snapshot_attempts     = 3;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_definitions_complete(0);
  ASSERT_EQ(fsm.state(), FeedState::SnapshotSync);

  const auto retry1 = fsm.on_tick(1000);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_TRUE(retry1.restart_snapshot);
  EXPECT_EQ(fsm.snapshot_attempts(), 1U);

  const auto retry2 = fsm.on_tick(2000);
  EXPECT_EQ(fsm.state(), FeedState::SnapshotSync);
  EXPECT_TRUE(retry2.restart_snapshot);
  EXPECT_EQ(fsm.snapshot_attempts(), 2U);

  fsm.on_tick(3000);
  EXPECT_EQ(fsm.state(), FeedState::Failed);
}

TEST(RecoveryFsmTest, SuccessfulCycleResetsTheAttemptCounter) {
  RecoveryConfig config;
  config.snapshot_cycle_timeout_ns = 1000;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_definitions_complete(0);
  fsm.on_tick(1000);
  ASSERT_EQ(fsm.snapshot_attempts(), 1U);
  fsm.on_snapshot_cycle_complete(1500);
  EXPECT_EQ(fsm.snapshot_attempts(), 0U);
}

TEST(RecoveryFsmTest, FailedIsAbsorbing) {
  RecoveryConfig config;
  config.definition_cycle_timeout_ns = 1;
  RecoveryFsm<RefVenue> fsm{config};
  fsm.start(0);
  fsm.on_tick(10);
  ASSERT_EQ(fsm.state(), FeedState::Failed);

  fsm.on_gap(GapRange{1, 2}, 11);
  EXPECT_EQ(fsm.state(), FeedState::Failed);
  fsm.on_window_blown(12);
  EXPECT_EQ(fsm.state(), FeedState::Failed);
  fsm.on_tick(13);
  EXPECT_EQ(fsm.state(), FeedState::Failed);
}

TEST(RecoveryFsmTest, OutOfOrderEventsAreIgnoredNotMisapplied) {
  RecoveryFsm<RefVenue> fsm;
  fsm.start(0);
  // A replay completion while awaiting definitions is nonsense; it must not
  // teleport the feed to Synchronized.
  fsm.on_replay_complete(1);
  EXPECT_EQ(fsm.state(), FeedState::AwaitingDefinitions);
  fsm.on_snapshot_cycle_complete(2);
  EXPECT_EQ(fsm.state(), FeedState::AwaitingDefinitions);
}

TEST(RecoveryFsmTest, TransitionsAreReportedExactlyOnce) {
  RecoveryFsm<RefVenue> fsm;
  fsm.start(0);
  auto t = fsm.take_transition();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->from, FeedState::Init);
  EXPECT_EQ(t->to, FeedState::AwaitingDefinitions);
  EXPECT_FALSE(fsm.take_transition().has_value());
}

TEST(RecoveryFsmTest, ReenteringTheSameStateIsNotATransition) {
  RecoveryFsm<RefVenue> fsm;
  fsm.start(0);
  fsm.on_definitions_complete(1);
  const auto before = fsm.transitions();
  fsm.on_window_blown(2);  // already SnapshotSync
  EXPECT_EQ(fsm.transitions(), before);
}

// ═════════════════════════════════════════════════════════════════════
// PacketBuffer
// ═════════════════════════════════════════════════════════════════════

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> vals) {
  std::vector<std::byte> out;
  for (const auto v : vals) out.push_back(static_cast<std::byte>(v));
  return out;
}

TEST(PacketBufferTest, PreservesOrderAndContents) {
  PacketBuffer buffer{4};
  const auto   a = bytes_of({1, 2, 3});
  const auto   b = bytes_of({4, 5});
  buffer.push(10, a, 100);
  buffer.push(11, b, 200);

  std::vector<SeqNum> seen;
  std::vector<std::size_t> sizes;
  buffer.for_each([&](const PacketBuffer::Entry& e) {
    seen.push_back(e.seq);
    sizes.push_back(e.bytes.size());
  });
  EXPECT_EQ(seen, (std::vector<SeqNum>{10, 11}));
  EXPECT_EQ(sizes, (std::vector<std::size_t>{3, 2}));
  EXPECT_EQ(buffer.size(), 2U);
}

TEST(PacketBufferTest, CopiesRatherThanBorrowing) {
  PacketBuffer buffer{4};
  auto         scratch = bytes_of({1, 2, 3});
  buffer.push(10, scratch, 100);
  // The transport recycles its receive buffer on the next poll(). If the buffer
  // borrowed, this would corrupt the replay.
  scratch[0] = std::byte{0xFF};

  buffer.for_each([](const PacketBuffer::Entry& e) { EXPECT_EQ(e.bytes[0], std::byte{1}); });
}

TEST(PacketBufferTest, OverflowDropsOldestAndCountsIt) {
  PacketBuffer buffer{2};
  const auto   payload = bytes_of({1});
  buffer.push(1, payload, 0);
  buffer.push(2, payload, 0);
  buffer.push(3, payload, 0);

  std::vector<SeqNum> seen;
  buffer.for_each([&](const PacketBuffer::Entry& e) { seen.push_back(e.seq); });
  // The newest packets are the ones no snapshot can reconstruct, so the oldest
  // goes. And it is counted — a silently truncated buffer reads as a clean
  // recovery when it was not.
  EXPECT_EQ(seen, (std::vector<SeqNum>{2, 3}));
  EXPECT_EQ(buffer.dropped(), 1U);
}

TEST(PacketBufferTest, ClearEmptiesWithoutReallocating) {
  PacketBuffer buffer{4};
  buffer.push(1, bytes_of({1}), 0);
  const auto capacity = buffer.capacity();
  buffer.clear();
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.capacity(), capacity);
}

TEST(PacketBufferTest, WrapsAroundRepeatedly) {
  PacketBuffer buffer{3};
  for (SeqNum s = 1; s <= 10; ++s) buffer.push(s, bytes_of({1}), 0);
  std::vector<SeqNum> seen;
  buffer.for_each([&](const PacketBuffer::Entry& e) { seen.push_back(e.seq); });
  EXPECT_EQ(seen, (std::vector<SeqNum>{8, 9, 10}));
  EXPECT_EQ(buffer.dropped(), 7U);
}

// ═════════════════════════════════════════════════════════════════════
// End to end: the loss-free baseline
// ═════════════════════════════════════════════════════════════════════

TEST(FeedEndToEnd, LossFreeRunSynchronizesAndMatchesTheReferenceModel) {
  TempCapture capture{"clean"};
  auto        params = base_params();
  test::SyntheticFeed feed{params};
  ASSERT_TRUE(feed.write(capture.path()).has_value());

  const auto run = run_capture(capture.path(), feed.securities());

  EXPECT_EQ(run.state, FeedState::Synchronized);
  EXPECT_EQ(run.stats.gaps, 0U);
  EXPECT_GT(run.stats.snapshot_cycles, 0U);
  EXPECT_EQ(run.builder.desyncs, 0U);
  EXPECT_EQ(run.builder.rpt_seq_gaps, 0U);
  EXPECT_EQ(run.builder.decode_errors, 0U);
  EXPECT_EQ(run.stats.framing_errors, 0U);
  EXPECT_EQ(run.stats.packet_rejects, 0U);

  EXPECT_TRUE(BooksMatch(run.books, model_state(feed, params.depth), "loss-free"));
}

TEST(FeedEndToEnd, ExercisesTheRecoveryReplayFilter) {
  // Guards the test itself: if the snapshot cycle were atomic, every buffered
  // packet would predate every snapshot, the replay would apply nothing, and
  // the gap tests below would pass without ever testing the join.
  TempCapture capture{"replayfilter"};
  auto        params = base_params();
  test::SyntheticFeed feed{params};
  ASSERT_TRUE(feed.write(capture.path()).has_value());

  const auto run = run_capture(capture.path(), feed.securities());
  EXPECT_GT(run.stats.replayed, 0U) << "no packets were replayed over a snapshot";
  EXPECT_GT(run.replay_skipped, 0U) << "the RptSeq join filter never rejected anything";
}

TEST(FeedEndToEnd, BothLinesCarryTheStreamRedundantly) {
  TempCapture capture{"lines"};
  auto        params = base_params();
  params.steps       = 200;
  test::SyntheticFeed feed{params};
  ASSERT_TRUE(feed.write(capture.path()).has_value());

  auto transport = PcapTransport::open(capture.path(), test::synthetic_config());
  ASSERT_TRUE(transport.has_value());
  CountingSink sink;
  ChannelHandler<RefVenue, PcapTransport, CountingSink> handler{310, *transport, sink};
  handler.start();
  handler.run();

  EXPECT_EQ(transport->unmatched(), 0U) << "capture contains traffic the config does not describe";
  EXPECT_GT(sink.instruments, 0U);
  EXPECT_GT(sink.books, 0U);
  EXPECT_FALSE(sink.transitions.empty());
  EXPECT_EQ(sink.transitions.back().to, FeedState::Synchronized);
}

TEST(FeedEndToEnd, SilentFeedGoesStaleRatherThanClaimingToBeLive) {
  TempCapture capture{"stale"};
  auto        params = base_params();
  params.steps       = 200;
  test::SyntheticFeed feed{params};
  ASSERT_TRUE(feed.write(capture.path()).has_value());

  auto transport = PcapTransport::open(capture.path(), test::synthetic_config());
  ASSERT_TRUE(transport.has_value());
  CountingSink sink;
  ChannelHandler<RefVenue, PcapTransport, CountingSink> handler{310, *transport, sink};
  handler.start();
  handler.run();

  ASSERT_EQ(handler.state(), FeedState::Synchronized);
  ASSERT_FALSE(handler.stale());
  const auto* book = handler.builder().book(feed.securities().front());
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(book->state, BookState::Live);

  // The venue goes away. No gap is ever declared — a gap needs a later sequence
  // number to reveal it, and none is coming. Only the clock can tell us.
  const Timestamp much_later = 1'700'000'000'000'000'000 + 3'600'000'000'000;
  handler.on_clock(much_later);

  EXPECT_TRUE(handler.stale());
  EXPECT_EQ(book->state, BookState::Stale);
  EXPECT_EQ(handler.stats().stale_periods, 1U);
  // Staleness is an alert, not a correctness event: the feed is still
  // Synchronized and nothing has been declared wrong.
  EXPECT_EQ(handler.state(), FeedState::Synchronized);
}

TEST(FeedEndToEnd, StalenessCanBeDisabled) {
  TempCapture capture{"nostale"};
  auto        params = base_params();
  params.steps       = 100;
  test::SyntheticFeed feed{params};
  ASSERT_TRUE(feed.write(capture.path()).has_value());

  auto transport = PcapTransport::open(capture.path(), test::synthetic_config());
  ASSERT_TRUE(transport.has_value());
  CountingSink  sink;
  HandlerConfig config;
  config.stale_after_ns = 0;
  ChannelHandler<RefVenue, PcapTransport, CountingSink> handler{310, *transport, sink, {}, config};
  handler.start();
  handler.run();
  handler.on_clock(1'700'000'000'000'000'000 + 3'600'000'000'000);
  EXPECT_FALSE(handler.stale());
}

// ═════════════════════════════════════════════════════════════════════
// The gate: every loss pattern converges to the loss-free book
// ═════════════════════════════════════════════════════════════════════

// Baseline plus one drop pattern, compared. The claim under test is convergence:
// whatever was lost, the handler must end up where the loss-free run ended up.
void ExpectConvergence(std::string_view name, const std::function<bool(SeqNum, LineId)>& drop,
                       bool expect_gap) {
  SCOPED_TRACE(name);

  TempCapture clean_capture{std::string{name} + "-clean"};
  TempCapture lossy_capture{std::string{name} + "-lossy"};

  auto params = base_params();

  test::SyntheticFeed clean{params};
  ASSERT_TRUE(clean.write(clean_capture.path()).has_value());

  // Same seed, so the two captures carry byte-identical market data. The only
  // difference is which packets survive.
  auto lossy_params = params;
  lossy_params.drop = drop;
  test::SyntheticFeed lossy{lossy_params};
  ASSERT_TRUE(lossy.write(lossy_capture.path()).has_value());

  const auto baseline = run_capture(clean_capture.path(), clean.securities());
  const auto recovered = run_capture(lossy_capture.path(), lossy.securities());

  ASSERT_EQ(baseline.state, FeedState::Synchronized);
  EXPECT_EQ(recovered.state, FeedState::Synchronized) << "did not re-synchronize";

  if (expect_gap) {
    EXPECT_GT(recovered.stats.gaps, 0U) << "expected a gap, but none was declared";
  } else {
    EXPECT_EQ(recovered.stats.gaps, 0U) << "declared a gap that redundancy should have covered";
  }

  EXPECT_TRUE(BooksMatch(recovered.books, baseline.books, "recovered vs loss-free"));
  EXPECT_TRUE(BooksMatch(recovered.books, model_state(lossy, params.depth), "recovered vs model"));
}

TEST(GapInjection, LineAFullyDownIsCoveredByLineB) {
  // The most common production incident. It must not be a book-correctness
  // event at all — B carries everything.
  ExpectConvergence(
      "line-a-down", [](SeqNum, LineId line) { return line == LineId::A; }, /*expect_gap=*/false);
}

TEST(GapInjection, LineBFullyDownIsCoveredByLineA) {
  ExpectConvergence(
      "line-b-down", [](SeqNum, LineId line) { return line == LineId::B; }, /*expect_gap=*/false);
}

TEST(GapInjection, AlternatingLineLossIsCoveredByArbitration) {
  // Neither line is complete, but together they are. Arbitration alone should
  // reconstruct the stream with no recovery whatsoever.
  ExpectConvergence(
      "alternating",
      [](SeqNum seq, LineId line) {
        return (seq % 2 == 0) ? line == LineId::A : line == LineId::B;
      },
      /*expect_gap=*/false);
}

TEST(GapInjection, SmallGapOnBothLinesRecoversViaSnapshot) {
  ExpectConvergence(
      "small-gap", [](SeqNum seq, LineId) { return seq >= 120 && seq <= 123; },
      /*expect_gap=*/true);
}

TEST(GapInjection, SinglePacketGapOnBothLinesRecovers) {
  ExpectConvergence(
      "one-packet", [](SeqNum seq, LineId) { return seq == 150; }, /*expect_gap=*/true);
}

TEST(GapInjection, RepeatedGapsRecoverEachTime) {
  ExpectConvergence(
      "repeated", [](SeqNum seq, LineId) { return seq % 97 == 0; }, /*expect_gap=*/true);
}

TEST(GapInjection, WindowBlowingGapRecovers) {
  // Larger than the arbiter's 256-packet reorder window, so there is no bounded
  // range to resync to — the handler has to abandon its cursor and rejoin from
  // scratch. The range must END before the capture does: a feed that never
  // resumes is a silent line, not a gap, and is a different failure entirely.
  ExpectConvergence(
      "window-blown", [](SeqNum seq, LineId) { return seq >= 100 && seq < 380; },
      /*expect_gap=*/true);
}

TEST(GapInjection, GapStraddlingASnapshotCycleRecovers) {
  // The nastiest ordinary case: loss begins before a snapshot cycle and ends
  // after it, so the cycle the handler joins is itself mid-flight.
  ExpectConvergence(
      "straddle", [](SeqNum seq, LineId) { return seq >= 158 && seq <= 168; },
      /*expect_gap=*/true);
}

TEST(GapInjection, LossAtTheVeryStartStillJoins) {
  ExpectConvergence(
      "cold-start", [](SeqNum seq, LineId) { return seq <= 30; }, /*expect_gap=*/false);
}

}  // namespace
}  // namespace rfeed
