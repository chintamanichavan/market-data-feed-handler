// Live-socket tests. These bind real sockets and send real multicast over the
// loopback interface, because the only bugs worth catching here are the ones a
// mock cannot have: a wrong bind address, a missing IGMP join, a control message
// parsed at the wrong offset.
//
// Every test skips rather than fails when the environment forbids multicast — a
// container without CAP_NET_ADMIN or with loopback multicast disabled is a fact
// about the machine, not a defect in the code.

#include <gtest/gtest.h>

#include "rfeed/transport_udp.hpp"

#ifdef __linux__

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "rfeed/pcap.hpp"
#include "ref_encoder.hpp"
#include "rfeed/feed_handler.hpp"
#include "ref_venue.hpp"

namespace rfeed {
namespace {

// Administratively-scoped groups (RFC 2365), high ports, so nothing on a real
// network could collide with a test run.
constexpr std::uint32_t kGroupA = ipv4(239, 255, 77, 11);
constexpr std::uint32_t kGroupB = ipv4(239, 255, 77, 12);
constexpr std::uint32_t kGroupUnjoined = ipv4(239, 255, 77, 99);
constexpr std::uint32_t kSnapA = ipv4(239, 255, 77, 21);
constexpr std::uint32_t kSnapB = ipv4(239, 255, 77, 22);
constexpr std::uint16_t kPortA  = 47311;
constexpr std::uint16_t kPortB  = 47312;
constexpr std::uint16_t kPortSnapA = 47321;
constexpr std::uint16_t kPortSnapB = 47322;

ChannelConfig loopback_config() {
  ChannelConfig cfg;
  cfg.id          = 900;
  cfg.name        = "loopback";
  cfg.connections = {
      {ChannelKind::Incremental, LineId::A, Endpoint{kGroupA, kPortA, 0}},
      {ChannelKind::Incremental, LineId::B, Endpoint{kGroupB, kPortB, 0}},
      {ChannelKind::Snapshot, LineId::A, Endpoint{kSnapA, kPortSnapA, 0}},
      {ChannelKind::Snapshot, LineId::B, Endpoint{kSnapB, kPortSnapB, 0}},
  };
  return cfg;
}

UdpConfig loopback_options() {
  UdpConfig options;
  options.interface    = INADDR_LOOPBACK;
  options.batch        = 8;
  options.rcvbuf_bytes = 1 << 20;
  return options;
}

// A multicast sender pinned to loopback.
class Sender {
 public:
  Sender() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return;
    in_addr iface{};
    iface.s_addr    = ::htonl(INADDR_LOOPBACK);
    const int loop  = 1;
    const int ttl   = 1;
    ok_ = ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) == 0 &&
          ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) == 0 &&
          ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) == 0;
  }
  ~Sender() {
    if (fd_ >= 0) ::close(fd_);
  }
  Sender(const Sender&)            = delete;
  Sender& operator=(const Sender&) = delete;

  [[nodiscard]] bool ok() const noexcept { return fd_ >= 0 && ok_; }

  bool send(std::uint32_t group, std::uint16_t port, std::span<const std::byte> payload) const {
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = ::htons(port);
    dst.sin_addr.s_addr = ::htonl(group);
    const auto sent = ::sendto(fd_, payload.data(), payload.size(), 0,
                               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    return sent == static_cast<ssize_t>(payload.size());
  }

 private:
  int  fd_{-1};
  bool ok_{false};
};

// Opens the transport, or skips the test with the reason. Returns nullopt when
// the environment cannot support it.
std::optional<UdpTransport> try_open() {
  auto transport = UdpTransport::open(loopback_config(), loopback_options());
  if (!transport) return std::nullopt;
  return std::move(*transport);
}

#define SKIP_UNLESS_MULTICAST(transport, sender)                                   \
  if (!(transport)) GTEST_SKIP() << "multicast unavailable in this environment";   \
  if (!(sender).ok()) GTEST_SKIP() << "cannot create a multicast sender"

// Polls until a packet arrives or the budget runs out. Loopback multicast is
// effectively synchronous, but not contractually so.
std::optional<RawPacket> poll_for(UdpTransport& transport, int budget_ms = 500) {
  for (int i = 0; i < budget_ms; ++i) {
    if (auto pkt = transport.poll()) return pkt;
    (void)transport.wait(1);
  }
  return std::nullopt;
}

std::vector<std::byte> payload_of(std::initializer_list<std::uint8_t> vals) {
  std::vector<std::byte> out;
  for (const auto v : vals) out.push_back(static_cast<std::byte>(v));
  return out;
}

TEST(UdpTransportTest, OpenReportsWhatTheKernelActuallyGranted) {
  auto transport = try_open();
  if (!transport) GTEST_SKIP() << "multicast unavailable in this environment";

  EXPECT_EQ(transport->socket_count(), 4U);
  EXPECT_FALSE(transport->exhausted());
  // net.core.rmem_max clamps this silently. Reporting the request rather than
  // the grant is how a handler ends up dropping at the open with a config file
  // that says 16MB.
  EXPECT_GT(transport->rcvbuf_granted(), 0);
}

TEST(UdpTransportTest, DeliversDatagramsTaggedWithLineAndKind) {
  auto   transport = try_open();
  Sender sender;
  SKIP_UNLESS_MULTICAST(transport, sender);

  const auto a = payload_of({0xAA, 0x01, 0x02});
  const auto b = payload_of({0xBB, 0x03});
  ASSERT_TRUE(sender.send(kGroupA, kPortA, a));
  ASSERT_TRUE(sender.send(kGroupB, kPortB, b));

  std::map<LineId, std::vector<std::byte>> got;
  for (int i = 0; i < 2; ++i) {
    const auto pkt = poll_for(*transport);
    ASSERT_TRUE(pkt.has_value()) << "no datagram " << i;
    EXPECT_EQ(pkt->kind, ChannelKind::Incremental);
    got[pkt->line].assign(pkt->bytes.begin(), pkt->bytes.end());
  }

  ASSERT_TRUE(got.contains(LineId::A));
  ASSERT_TRUE(got.contains(LineId::B));
  EXPECT_EQ(got[LineId::A], a);
  EXPECT_EQ(got[LineId::B], b);
  EXPECT_EQ(transport->stats().datagrams, 2U);
}

TEST(UdpTransportTest, BindsPerGroupSoChannelsDoNotCrossFeed) {
  auto   transport = try_open();
  Sender sender;
  SKIP_UNLESS_MULTICAST(transport, sender);

  // Sent to a group the transport never joined. Binding to INADDR_ANY instead
  // of the group would deliver this on whichever socket shares the port, and
  // the packet would be attributed to the wrong line.
  ASSERT_TRUE(sender.send(kGroupUnjoined, kPortA, payload_of({0xCC})));

  // Then something legitimate, so we are not just measuring an empty socket.
  ASSERT_TRUE(sender.send(kGroupA, kPortA, payload_of({0xAA})));

  const auto pkt = poll_for(*transport);
  ASSERT_TRUE(pkt.has_value());
  EXPECT_EQ(pkt->bytes.size(), 1U);
  EXPECT_EQ(pkt->bytes[0], std::byte{0xAA});
  EXPECT_EQ(transport->stats().datagrams, 1U);
}

TEST(UdpTransportTest, StampsEveryPacketWithAUsableTimestamp) {
  auto   transport = try_open();
  Sender sender;
  SKIP_UNLESS_MULTICAST(transport, sender);

  ASSERT_TRUE(sender.send(kGroupA, kPortA, payload_of({1, 2, 3, 4})));
  const auto pkt = poll_for(*transport);
  ASSERT_TRUE(pkt.has_value());

  // Whatever the source, rx_ts must be a plausible epoch-nanosecond value —
  // downstream deadline arithmetic in the recovery FSM depends on it.
  EXPECT_GT(pkt->rx_ts, 1'600'000'000'000'000'000);
  EXPECT_NE(transport->timestamp_source(), TimestampSource::Userspace)
      << "fell back to userspace timestamps; latency numbers would be meaningless";
}

TEST(UdpTransportTest, BatchesMultipleDatagramsInOneSyscall) {
  auto   transport = try_open();
  Sender sender;
  SKIP_UNLESS_MULTICAST(transport, sender);

  constexpr int kCount = 6;  // below the batch size of 8
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(sender.send(kGroupA, kPortA, payload_of({static_cast<std::uint8_t>(i)})));
  }

  std::vector<std::uint8_t> seen;
  for (int i = 0; i < kCount; ++i) {
    const auto pkt = poll_for(*transport);
    if (!pkt) break;
    seen.push_back(static_cast<std::uint8_t>(pkt->bytes[0]));
  }

  ASSERT_EQ(seen.size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) EXPECT_EQ(seen[static_cast<std::size_t>(i)], i);
  // The whole point of recvmmsg: far fewer syscalls than datagrams.
  EXPECT_LT(transport->stats().batches, static_cast<std::uint64_t>(kCount));
}

TEST(UdpTransportTest, PollReturnsNulloptWhenIdleRatherThanBlocking) {
  auto transport = try_open();
  if (!transport) GTEST_SKIP() << "multicast unavailable in this environment";
  // Non-blocking is not an optimisation here: a blocking poll() would stop
  // ChannelHandler from ever running its staleness and timeout checks, so a
  // dead feed would hang instead of being reported.
  EXPECT_FALSE(transport->poll().has_value());
}

TEST(UdpTransportTest, RejectsAConfigWithNoMulticastEndpoints) {
  ChannelConfig cfg;
  cfg.id   = 901;
  cfg.name = "replay-only";
  // TCP replay is ReplayClient's job; a config with nothing but that is not
  // something this transport can serve.
  cfg.connections = {{ChannelKind::HistoricalReplay, LineId::A, Endpoint{0x7F000001, 5000, 0}}};

  const auto transport = UdpTransport::open(cfg, loopback_options());
  ASSERT_FALSE(transport.has_value());
  EXPECT_EQ(transport.error(), TransportError::NoConnections);
}

TEST(UdpTransportTest, MovingDoesNotCloseTheSockets) {
  auto   transport = try_open();
  Sender sender;
  SKIP_UNLESS_MULTICAST(transport, sender);

  UdpTransport moved = std::move(*transport);
  ASSERT_TRUE(sender.send(kGroupA, kPortA, payload_of({0x42})));

  const auto pkt = poll_for(moved);
  ASSERT_TRUE(pkt.has_value());
  EXPECT_EQ(pkt->bytes[0], std::byte{0x42});
}

// The point of the Transport concept: the same handler, unmodified, over a
// socket instead of a file. This drives a full recovery join — buffer an
// incremental, join the snapshot loop, replay the buffer on top — entirely
// across real multicast.
TEST(UdpTransportTest, DrivesTheFeedHandlerThroughARealJoin) {
  auto   transport = try_open();
  Sender sender;
  SKIP_UNLESS_MULTICAST(transport, sender);

  struct NullSink {
    std::uint64_t events{0};
    void operator()(const NormalizedEvent&) noexcept { ++events; }
  };

  NullSink       sink;
  RecoveryConfig recovery;
  recovery.has_definition_channel = false;  // this config has no definition loop
  ChannelHandler<venue_ref::RefVenue, UdpTransport, NullSink> handler{900, *transport, sink,
                                                                      recovery};
  handler.start();

  MdEntry entry{};
  entry.px          = Mantissa{100'000'000};
  entry.size        = 5;
  entry.security    = 42;
  entry.rpt_seq     = 1;
  entry.num_orders  = 1;
  entry.price_level = 1;
  entry.action      = UpdateAction::New;
  entry.type        = EntryType::Bid;

  const auto incremental = venue_ref::encode_incremental_packet(
      1, 1'700'000'000'000'000'000, IncrementalHeader{.transact_time = 1, .end_of_event = true},
      std::span{&entry, 1});
  ASSERT_TRUE(sender.send(kGroupA, kPortA, incremental));

  auto pump = [&](int iterations) {
    for (int i = 0; i < iterations; ++i) {
      (void)transport->wait(1);
      while (handler.step()) {
      }
    }
  };
  pump(50);

  // Still joining, so the incremental is held rather than applied. A book here
  // would mean the handler was publishing state it had no snapshot to anchor.
  ASSERT_EQ(handler.state(), FeedState::SnapshotSync);
  EXPECT_GT(handler.stats().buffered, 0U);
  EXPECT_EQ(handler.builder().book(42), nullptr);

  // An empty snapshot at RptSeq 0 — the instrument had no state as of sequence
  // 0. TotNumReports of 1 completes the cycle on this single report.
  const SnapshotHeader header{
      .last_msg_seq_num_processed = 0,
      .tot_num_reports            = 1,
      .security                   = 42,
      .rpt_seq                    = 0,
      .transact_time              = 1,
      .status                     = TradingStatus::Open,
  };
  venue_ref::Encoder enc{1, 1'700'000'000'000'000'001};
  enc.snapshot(header, {});
  ASSERT_TRUE(sender.send(kSnapA, kPortSnapA, enc.view()));
  pump(50);

  EXPECT_EQ(handler.state(), FeedState::Synchronized);
  EXPECT_GT(handler.stats().snapshot_cycles, 0U);
  EXPECT_GT(handler.stats().replayed, 0U);

  // The buffered incremental carried RptSeq 1, past the snapshot's 0, so it
  // replays on top rather than being skipped as already-included.
  const auto* book = handler.builder().book(42);
  ASSERT_NE(book, nullptr);
  ASSERT_EQ(book->outright.bids().size(), 1U);
  EXPECT_EQ(book->outright.bids()[0].px.raw, 100'000'000);
  EXPECT_EQ(book->outright.bids()[0].qty, 5);
  EXPECT_EQ(book->state, BookState::Live);
  EXPECT_EQ(handler.stats().packet_rejects, 0U);
  EXPECT_EQ(handler.stats().framing_errors, 0U);
  EXPECT_EQ(handler.stats().message_errors, 0U);
}

}  // namespace
}  // namespace rfeed

#endif  // __linux__
