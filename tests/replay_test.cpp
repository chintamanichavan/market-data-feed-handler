// TCP replay: the rate-limit policy, the wire framing, and the client driven
// against a real loopback server.
//
// The limiter gets the most attention because its failure mode is the worst one
// in the project: a gap triggers a replay, the venue refuses, the refusal is
// retried, and the handler hammers the venue exactly when the feed is already
// unhealthy. That has to be impossible by construction, not by convention.

#include <gtest/gtest.h>

#include "ref_replay.hpp"
#include "ref_venue.hpp"
#include "rfeed/replay.hpp"
#include "rfeed/replay_client.hpp"

#ifdef __linux__

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "ref_encoder.hpp"
#include "rfeed/packet.hpp"

namespace rfeed {
namespace {

using venue_ref::ReplayParser;
using venue_ref::ReplayStatus;
using venue_ref::RefVenue;

using Client = ReplayClient<RefVenue, ReplayParser>;

constexpr Timestamp kSecond = 1'000'000'000;

// ═════════════════════════════════════════════════════════════════════
// ReplayLimiter — pure policy, no sockets
// ═════════════════════════════════════════════════════════════════════

TEST(ReplayLimiterTest, GrantsAReasonableRequest) {
  ReplayLimiter<RefVenue> limiter;
  EXPECT_TRUE(limiter.acquire(GapRange{100, 109}, kSecond).has_value());
  EXPECT_EQ(limiter.in_flight(), 1U);
  EXPECT_EQ(limiter.requests_today(), 1U);
}

TEST(ReplayLimiterTest, RejectsAnUnboundedRange) {
  ReplayLimiter<RefVenue> limiter;
  // end == begin - 1 is the arbiter's encoding for "we do not know how far the
  // hole goes". Turning that into a request is how a session gets terminated.
  const auto ok = limiter.check(GapRange{100, 99}, kSecond);
  ASSERT_FALSE(ok.has_value());
  EXPECT_EQ(ok.error(), ReplayReject::RangeEmpty);
}

TEST(ReplayLimiterTest, RejectsARangeBeyondThePerRequestCap) {
  ReplayLimiter<RefVenue> limiter;
  const SeqNum begin = 100;
  const SeqNum end   = begin + RefVenue::kReplayLimits.max_packets_per_request;
  const auto   ok    = limiter.check(GapRange{begin, end}, kSecond);
  ASSERT_FALSE(ok.has_value());
  EXPECT_EQ(ok.error(), ReplayReject::RangeTooLarge);
}

TEST(ReplayLimiterTest, AcceptsExactlyTheCap) {
  ReplayLimiter<RefVenue> limiter;
  const SeqNum begin = 100;
  const SeqNum end   = begin + RefVenue::kReplayLimits.max_packets_per_request - 1;
  const GapRange at_cap{begin, end};
  EXPECT_EQ(at_cap.count(), RefVenue::kReplayLimits.max_packets_per_request);
  EXPECT_TRUE(limiter.check(GapRange{begin, end}, kSecond).has_value());
}

TEST(ReplayLimiterTest, EnforcesTheConcurrencyCap) {
  ReplayLimiter<RefVenue> limiter;
  ASSERT_TRUE(limiter.acquire(GapRange{1, 2}, kSecond).has_value());
  const auto second = limiter.acquire(GapRange{10, 11}, 100 * kSecond);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error(), ReplayReject::TooManyInFlight);

  limiter.release();
  EXPECT_TRUE(limiter.acquire(GapRange{10, 11}, 100 * kSecond).has_value());
}

TEST(ReplayLimiterTest, EnforcesTheCooldown) {
  ReplayLimiter<RefVenue> limiter;
  ASSERT_TRUE(limiter.acquire(GapRange{1, 2}, 10 * kSecond).has_value());
  limiter.release();

  // Immediately again: refused, and the caller is told how long to wait rather
  // than being left to poll.
  const auto tooSoon = limiter.check(GapRange{3, 4}, 10 * kSecond);
  ASSERT_FALSE(tooSoon.has_value());
  EXPECT_EQ(tooSoon.error(), ReplayReject::Cooldown);
  EXPECT_GT(limiter.retry_after(10 * kSecond), 0);

  const Timestamp after = 10 * kSecond +
                          static_cast<Timestamp>(RefVenue::kReplayLimits.cooldown.count()) *
                              1'000'000;
  EXPECT_TRUE(limiter.check(GapRange{3, 4}, after).has_value());
  EXPECT_EQ(limiter.retry_after(after), 0);
}

TEST(ReplayLimiterTest, EnforcesTheDailyQuota) {
  ReplayLimiter<RefVenue> limiter;
  Timestamp               now = kSecond;
  for (std::uint32_t i = 0; i < RefVenue::kReplayLimits.max_requests_per_day; ++i) {
    ASSERT_TRUE(limiter.acquire(GapRange{1, 2}, now).has_value()) << "request " << i;
    limiter.release();
    now += 10 * kSecond;
  }
  const auto over = limiter.acquire(GapRange{1, 2}, now);
  ASSERT_FALSE(over.has_value());
  EXPECT_EQ(over.error(), ReplayReject::DailyQuotaSpent);

  // The venue's day rolls on its own schedule, not local midnight, so this is
  // an explicit call rather than something inferred from a clock.
  limiter.new_trading_day();
  EXPECT_TRUE(limiter.acquire(GapRange{1, 2}, now).has_value());
}

TEST(ReplayLimiterTest, CheckDoesNotSpendBudget) {
  ReplayLimiter<RefVenue> limiter;
  for (int i = 0; i < 50; ++i) {
    EXPECT_TRUE(limiter.check(GapRange{1, 2}, kSecond).has_value());
  }
  EXPECT_EQ(limiter.requests_today(), 0U);
  EXPECT_EQ(limiter.stats().granted, 0U);
}

TEST(ReplayLimiterTest, CountsRejections) {
  ReplayLimiter<RefVenue> limiter;
  ASSERT_FALSE(limiter.acquire(GapRange{100, 99}, kSecond).has_value());
  EXPECT_EQ(limiter.stats().rejected, 1U);
  EXPECT_EQ(limiter.stats().granted, 0U);
}

// ═════════════════════════════════════════════════════════════════════
// Wire framing
// ═════════════════════════════════════════════════════════════════════

TEST(ReplayProtocolTest, RequestRoundTrips) {
  std::vector<std::byte> out;
  ReplayParser::encode_request(out, 310, GapRange{1000, 1049});
  ASSERT_EQ(out.size(), venue_ref::kReplayRequestBytes);

  const auto request = ReplayParser::decode_request(out);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->channel, 310);
  EXPECT_EQ(request->begin, 1000U);
  EXPECT_EQ(request->count, 50U);
}

TEST(ReplayProtocolTest, RejectsARequestWithTheWrongMagic) {
  std::vector<std::byte> out(venue_ref::kReplayRequestBytes, std::byte{0});
  EXPECT_FALSE(ReplayParser::decode_request(out).has_value());
}

std::vector<std::byte> build_response(ReplayStatus status,
                                      const std::vector<std::vector<std::byte>>& packets) {
  std::vector<std::byte> out;
  ReplayParser::encode_response_header(out, status,
                                       static_cast<std::uint32_t>(packets.size()));
  for (const auto& p : packets) ReplayParser::encode_record(out, p);
  return out;
}

std::vector<std::byte> fake_datagram(std::uint8_t tag, std::size_t len) {
  return std::vector<std::byte>(len, static_cast<std::byte>(tag));
}

TEST(ReplayProtocolTest, ParsesAWholeResponse) {
  const std::vector<std::vector<std::byte>> packets{fake_datagram(1, 20), fake_datagram(2, 33)};
  const auto                                wire = build_response(ReplayStatus::Ok, packets);

  ReplayParser parser;
  ASSERT_TRUE(parser.consume(wire));
  ASSERT_TRUE(parser.complete());
  EXPECT_EQ(parser.status(), ReplayStatus::Ok);
  ASSERT_EQ(parser.packets().size(), 2U);
  EXPECT_EQ(parser.packets()[0], packets[0]);
  EXPECT_EQ(parser.packets()[1], packets[1]);
}

TEST(ReplayProtocolTest, SurvivesBeingSplitAtEveryByteBoundary) {
  const std::vector<std::vector<std::byte>> packets{fake_datagram(7, 40), fake_datagram(8, 12)};
  const auto                                wire = build_response(ReplayStatus::Ok, packets);

  // TCP has no respect for message boundaries. A parser that only works when a
  // response arrives whole passes every test until the day a response exceeds
  // one segment — which is the day it matters.
  for (std::size_t split = 1; split < wire.size(); ++split) {
    ReplayParser parser;
    ASSERT_TRUE(parser.consume(std::span{wire}.first(split))) << "split at " << split;
    ASSERT_TRUE(parser.consume(std::span{wire}.subspan(split))) << "split at " << split;
    ASSERT_TRUE(parser.complete()) << "split at " << split;
    ASSERT_EQ(parser.packets().size(), 2U) << "split at " << split;
    EXPECT_EQ(parser.packets()[0], packets[0]) << "split at " << split;
    EXPECT_EQ(parser.packets()[1], packets[1]) << "split at " << split;
  }
}

TEST(ReplayProtocolTest, OneByteAtATimeStillParses) {
  const std::vector<std::vector<std::byte>> packets{fake_datagram(3, 17)};
  const auto                                wire = build_response(ReplayStatus::Ok, packets);

  ReplayParser parser;
  for (const auto b : wire) {
    ASSERT_TRUE(parser.consume(std::span{&b, 1}));
  }
  ASSERT_TRUE(parser.complete());
  EXPECT_EQ(parser.packets()[0], packets[0]);
}

TEST(ReplayProtocolTest, RefusalCarriesNoRecords) {
  const auto   wire = build_response(ReplayStatus::RateLimited, {});
  ReplayParser parser;
  ASSERT_TRUE(parser.consume(wire));
  EXPECT_EQ(parser.status(), ReplayStatus::RateLimited);
  // Complete, not pending: waiting for a body that will never arrive is how a
  // client hangs until its timeout on every refusal.
  EXPECT_TRUE(parser.complete());
  EXPECT_EQ(parser.expected_packets(), 0U);
}

TEST(ReplayProtocolTest, RejectsBadMagic) {
  auto wire = build_response(ReplayStatus::Ok, {});
  wire[0]   = std::byte{'X'};
  ReplayParser parser;
  EXPECT_FALSE(parser.consume(wire));
}

TEST(ReplayProtocolTest, RejectsAnUnknownStatusRatherThanGuessing) {
  std::vector<std::byte> wire;
  ReplayParser::encode_response_header(wire, static_cast<ReplayStatus>(99), 0);
  ReplayParser parser;
  EXPECT_FALSE(parser.consume(wire));
}

TEST(ReplayProtocolTest, RejectsAnOversizedRecord) {
  std::vector<std::byte> wire;
  ReplayParser::encode_response_header(wire, ReplayStatus::Ok, 1);
  const std::uint16_t huge = 60'000;  // beyond kMaxDatagramBytes
  wire.push_back(static_cast<std::byte>(huge & 0xFF));
  wire.push_back(static_cast<std::byte>(huge >> 8));

  ReplayParser parser;
  EXPECT_FALSE(parser.consume(wire));
}

TEST(ReplayProtocolTest, ResetClearsEverything) {
  ReplayParser parser;
  ASSERT_TRUE(parser.consume(build_response(ReplayStatus::Ok, {fake_datagram(1, 8)})));
  ASSERT_TRUE(parser.complete());
  parser.reset();
  EXPECT_FALSE(parser.header_seen());
  EXPECT_TRUE(parser.packets().empty());
}

// ═════════════════════════════════════════════════════════════════════
// Client against a real loopback server
// ═════════════════════════════════════════════════════════════════════

struct ServerBehaviour {
  ReplayStatus  status{ReplayStatus::Ok};
  std::uint32_t claim_packets{0};   // what the header says
  std::uint32_t send_packets{0};    // what it actually sends
  std::size_t   payload_len{32};
  bool          dribble{false};     // one byte per write, to force partial reads
  bool          never_respond{false};
};

// Single-connection TCP server on 127.0.0.1, run on its own thread. Real
// sockets, because the bugs worth catching here — partial writes, orderly
// close, connect races — cannot exist in a mock.
class TestReplayServer {
 public:
  explicit TestReplayServer(ServerBehaviour behaviour) : behaviour_{behaviour} {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;
    const int on = 1;
    (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;  // ephemeral
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
    if (::listen(listen_fd_, 1) != 0) return;

    sockaddr_in bound{};
    socklen_t   len = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) return;
    port_ = ::ntohs(bound.sin_port);
    ok_   = true;

    thread_ = std::thread([this] { serve(); });
  }

  ~TestReplayServer() {
    stop_ = true;
    if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
  }

  TestReplayServer(const TestReplayServer&)            = delete;
  TestReplayServer& operator=(const TestReplayServer&) = delete;

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] ReplayParser::Request last_request() const {
    return ReplayParser::Request{.channel = static_cast<ChannelId>(req_channel_.load()),
                                 .begin   = req_begin_.load(),
                                 .count   = req_count_.load()};
  }

 private:
  void serve() {
    const int conn = ::accept(listen_fd_, nullptr, nullptr);
    if (conn < 0) return;

    std::array<std::byte, 64> request{};
    const auto n = ::recv(conn, request.data(), venue_ref::kReplayRequestBytes, MSG_WAITALL);
    if (n == static_cast<ssize_t>(venue_ref::kReplayRequestBytes)) {
      if (const auto parsed = ReplayParser::decode_request(request)) {
        req_channel_.store(parsed->channel);
        req_begin_.store(parsed->begin);
        req_count_.store(parsed->count);
      }
    }

    if (behaviour_.never_respond) {
      while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds{5});
      ::close(conn);
      return;
    }

    std::vector<std::byte> wire;
    ReplayParser::encode_response_header(wire, behaviour_.status, behaviour_.claim_packets);
    for (std::uint32_t i = 0; i < behaviour_.send_packets; ++i) {
      ReplayParser::encode_record(
          wire, std::vector<std::byte>(behaviour_.payload_len, static_cast<std::byte>(i + 1)));
    }

    if (behaviour_.dribble) {
      for (const auto b : wire) {
        if (::send(conn, &b, 1, MSG_NOSIGNAL) != 1) break;
      }
    } else {
      std::size_t sent = 0;
      while (sent < wire.size()) {
        const auto written = ::send(conn, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) break;
        sent += static_cast<std::size_t>(written);
      }
    }
    ::close(conn);
  }

  ServerBehaviour                      behaviour_;
  std::thread                          thread_{};
  std::atomic<std::uint32_t>           req_channel_{0};
  std::atomic<std::uint32_t>           req_begin_{0};
  std::atomic<std::uint32_t>           req_count_{0};
  std::atomic<bool>                    stop_{false};
  int                                  listen_fd_{-1};
  std::uint16_t                        port_{0};
  bool                                 ok_{false};
};

// Drives the client to a terminal state, advancing a synthetic clock.
ReplayPhase pump(Client& client, Timestamp start, Timestamp step = 1'000'000,
                 int max_iterations = 5000) {
  Timestamp now = start;
  for (int i = 0; i < max_iterations; ++i) {
    client.poll(now);
    if (!client.busy()) return client.phase();
    now += step;
    std::this_thread::sleep_for(std::chrono::microseconds{200});
  }
  return client.phase();
}

TEST(ReplayClientTest, RecoversAFullRange) {
  TestReplayServer server{ServerBehaviour{.status        = ReplayStatus::Ok,
                                          .claim_packets = 4,
                                          .send_packets  = 4,
                                          .payload_len   = 48}};
  ASSERT_TRUE(server.ok());

  Client client{Endpoint{INADDR_LOOPBACK, server.port(), 0}, 310};
  ASSERT_TRUE(client.request(GapRange{500, 503}, kSecond).has_value());

  ASSERT_EQ(pump(client, kSecond), ReplayPhase::Complete);
  EXPECT_EQ(client.recovered(), 4U);
  EXPECT_EQ(client.stats().completed, 1U);
  EXPECT_EQ(client.stats().packets_recovered, 4U);

  // The request the venue actually saw must match the gap the arbiter reported.
  const auto request = server.last_request();
  EXPECT_EQ(request.channel, 310);
  EXPECT_EQ(request.begin, 500U);
  EXPECT_EQ(request.count, 4U);

  std::size_t drained = 0;
  client.drain([&](std::span<const std::byte> packet) {
    EXPECT_EQ(packet.size(), 48U);
    ++drained;
  });
  EXPECT_EQ(drained, 4U);
  // Drained and back to Idle, with the concurrency slot returned.
  EXPECT_EQ(client.phase(), ReplayPhase::Idle);
  EXPECT_EQ(client.limiter().in_flight(), 0U);
}

TEST(ReplayClientTest, HandlesAResponseDribbledOneByteAtATime) {
  TestReplayServer server{ServerBehaviour{.status        = ReplayStatus::Ok,
                                          .claim_packets = 3,
                                          .send_packets  = 3,
                                          .payload_len   = 24,
                                          .dribble       = true}};
  ASSERT_TRUE(server.ok());

  Client client{Endpoint{INADDR_LOOPBACK, server.port(), 0}, 310};
  ASSERT_TRUE(client.request(GapRange{10, 12}, kSecond).has_value());
  ASSERT_EQ(pump(client, kSecond), ReplayPhase::Complete);
  EXPECT_EQ(client.recovered(), 3U);
}

TEST(ReplayClientTest, TreatsARefusalAsAFailureNotAnEmptySuccess) {
  TestReplayServer server{ServerBehaviour{.status = ReplayStatus::RateLimited}};
  ASSERT_TRUE(server.ok());

  Client client{Endpoint{INADDR_LOOPBACK, server.port(), 0}, 310};
  ASSERT_TRUE(client.request(GapRange{10, 12}, kSecond).has_value());

  EXPECT_EQ(pump(client, kSecond), ReplayPhase::Failed);
  EXPECT_EQ(client.failure(), ReplayFailure::RefusedByVenue);
  // Failing must return the concurrency slot, or one refusal permanently
  // disables replay for the rest of the session.
  EXPECT_EQ(client.limiter().in_flight(), 0U);
}

TEST(ReplayClientTest, ShortResponseIsAFailureNotAPartialFill) {
  // The venue claims four packets and sends two, then closes. Accepting this
  // would leave a hole the handler believes it filled — silently wrong books.
  TestReplayServer server{ServerBehaviour{.status        = ReplayStatus::Ok,
                                          .claim_packets = 4,
                                          .send_packets  = 2}};
  ASSERT_TRUE(server.ok());

  Client client{Endpoint{INADDR_LOOPBACK, server.port(), 0}, 310};
  ASSERT_TRUE(client.request(GapRange{10, 13}, kSecond).has_value());

  EXPECT_EQ(pump(client, kSecond), ReplayPhase::Failed);
  EXPECT_EQ(client.failure(), ReplayFailure::ShortResponse);
  EXPECT_EQ(client.stats().completed, 0U);
}

TEST(ReplayClientTest, FewerPacketsThanTheGapIsAFailure) {
  // Header and body agree, but the venue served a narrower range than asked
  // for. Still a hole.
  TestReplayServer server{ServerBehaviour{.status        = ReplayStatus::Ok,
                                          .claim_packets = 2,
                                          .send_packets  = 2}};
  ASSERT_TRUE(server.ok());

  Client client{Endpoint{INADDR_LOOPBACK, server.port(), 0}, 310};
  ASSERT_TRUE(client.request(GapRange{10, 13}, kSecond).has_value());  // asked for 4

  EXPECT_EQ(pump(client, kSecond), ReplayPhase::Failed);
  EXPECT_EQ(client.failure(), ReplayFailure::ShortResponse);
}

TEST(ReplayClientTest, TimesOutOnASilentVenue) {
  TestReplayServer server{ServerBehaviour{.never_respond = true}};
  ASSERT_TRUE(server.ok());

  Client client{Endpoint{INADDR_LOOPBACK, server.port(), 0}, 310, 50'000'000};  // 50ms
  ASSERT_TRUE(client.request(GapRange{10, 12}, kSecond).has_value());

  // A replay that never answers must not hold the recovery path open forever.
  EXPECT_EQ(pump(client, kSecond, 10'000'000), ReplayPhase::Failed);
  EXPECT_EQ(client.failure(), ReplayFailure::Timeout);
  EXPECT_EQ(client.limiter().in_flight(), 0U);
}

TEST(ReplayClientTest, ConnectionRefusedFailsCleanly) {
  // Nothing is listening on this port.
  Client client{Endpoint{INADDR_LOOPBACK, 1, 0}, 310, 200'000'000};
  ASSERT_TRUE(client.request(GapRange{10, 12}, kSecond).has_value());

  const auto phase = pump(client, kSecond, 10'000'000);
  EXPECT_EQ(phase, ReplayPhase::Failed);
  EXPECT_EQ(client.limiter().in_flight(), 0U);
}

TEST(ReplayClientTest, RefusesASecondRequestWhileOneIsInFlight) {
  TestReplayServer server{ServerBehaviour{.never_respond = true}};
  ASSERT_TRUE(server.ok());

  Client client{Endpoint{INADDR_LOOPBACK, server.port(), 0}, 310, kSecond};
  ASSERT_TRUE(client.request(GapRange{10, 12}, kSecond).has_value());

  const auto second = client.request(GapRange{20, 22}, 100 * kSecond);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error(), ReplayReject::TooManyInFlight);
}

TEST(ReplayClientTest, BudgetIsCheckedBeforeAnySocketIsOpened) {
  // No server at all. The request must be rejected on policy alone, without a
  // connection attempt — the venue never learns we wanted something we are not
  // allowed to ask for.
  Client client{Endpoint{INADDR_LOOPBACK, 1, 0}, 310};
  const auto rejected = client.request(GapRange{10, 9}, kSecond);  // unbounded
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error(), ReplayReject::RangeEmpty);
  EXPECT_EQ(client.phase(), ReplayPhase::Idle);
  EXPECT_EQ(client.stats().requests, 0U);
}

TEST(ReplayClientTest, RecoveredPacketsRejoinTheNormalDecodePath) {
  // The point of carrying whole datagrams over the replay channel: a recovered
  // packet is byte-identical to the multicast one, so it shares every line of
  // framing and decoding rather than needing a parallel path.
  MdEntry entry{};
  entry.px          = Mantissa{123'000'000};
  entry.size        = 9;
  entry.security    = 77;
  entry.rpt_seq     = 4;
  entry.num_orders  = 1;
  entry.price_level = 1;
  entry.action      = UpdateAction::New;
  entry.type        = EntryType::Bid;

  const auto datagram = venue_ref::encode_incremental_packet(
      501, 1'700'000'000'000'000'000,
      IncrementalHeader{.transact_time = 1, .end_of_event = true}, std::span{&entry, 1});

  std::vector<std::byte> wire;
  ReplayParser::encode_response_header(wire, ReplayStatus::Ok, 1);
  ReplayParser::encode_record(wire, datagram);

  ReplayParser parser;
  ASSERT_TRUE(parser.consume(wire));
  ASSERT_TRUE(parser.complete());
  ASSERT_EQ(parser.packets().size(), 1U);

  const auto& recovered = parser.packets()[0];
  const auto  packet    = PacketView::decode(recovered);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.seq, 501U);
  EXPECT_FALSE(packet->is_heartbeat());
}

}  // namespace
}  // namespace rfeed

#endif  // __linux__
