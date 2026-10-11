#include "rfeed/pcap.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "rfeed/channel_config.hpp"
#include "rfeed/transport_pcap.hpp"

namespace {

using namespace rfeed;

std::string temp_path(const std::string& name) {
  return ::testing::TempDir() + "/rfeed_" + name + ".pcap";
}

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> octets) {
  std::vector<std::byte> out;
  for (const auto o : octets) out.push_back(static_cast<std::byte>(o));
  return out;
}

constexpr std::uint32_t kGroupIncrA = ipv4(224, 0, 1, 1);
constexpr std::uint32_t kGroupIncrB = ipv4(224, 0, 1, 2);
constexpr std::uint32_t kGroupSnapA = ipv4(224, 0, 1, 3);
constexpr std::uint16_t kPortIncrA  = 14310;
constexpr std::uint16_t kPortIncrB  = 15310;
constexpr std::uint16_t kPortSnapA  = 14311;

ChannelConfig ref_config() {
  ChannelConfig cfg;
  cfg.id   = 310;
  cfg.name = "ref-310";
  cfg.connections = {
      ConnectionConfig{ChannelKind::Incremental, LineId::A, Endpoint{kGroupIncrA, kPortIncrA, 0}},
      ConnectionConfig{ChannelKind::Incremental, LineId::B, Endpoint{kGroupIncrB, kPortIncrB, 0}},
      ConnectionConfig{ChannelKind::Snapshot, LineId::A, Endpoint{kGroupSnapA, kPortSnapA, 0}},
  };
  return cfg;
}

TEST(Pcap, WriteThenReadRoundTripsPayloadAndTimestamp) {
  const auto path    = temp_path("roundtrip");
  const auto payload = bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02});

  {
    auto writer = PcapWriter::create(path);
    ASSERT_TRUE(writer.has_value()) << to_string(writer.error());
    ASSERT_TRUE(writer->write_udp(payload, 1'700'000'000'123'456'789, ipv4(192, 0, 2, 10), 40000,
                                  kGroupIncrA, kPortIncrA)
                    .has_value());
    ASSERT_TRUE(writer->close().has_value());
  }

  auto reader = PcapReader::open(path);
  ASSERT_TRUE(reader.has_value()) << to_string(reader.error());

  const auto dgram = reader->next();
  ASSERT_TRUE(dgram.has_value());
  EXPECT_EQ(dgram->rx_ts, 1'700'000'000'123'456'789);
  EXPECT_EQ(dgram->dst_addr, kGroupIncrA);
  EXPECT_EQ(dgram->dst_port, kPortIncrA);
  EXPECT_EQ(dgram->src_port, 40000);
  ASSERT_EQ(dgram->payload.size(), payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), dgram->payload.begin()));

  EXPECT_FALSE(reader->next().has_value());
  EXPECT_EQ(reader->records_read(), 1U);
  EXPECT_EQ(reader->records_skipped(), 0U);
}

TEST(Pcap, EmptyPayloadIsPreserved) {
  const auto path = temp_path("empty_payload");
  {
    auto writer = PcapWriter::create(path);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(
        writer->write_udp({}, 1000, ipv4(192, 0, 2, 10), 1, kGroupIncrA, kPortIncrA).has_value());
    ASSERT_TRUE(writer->close().has_value());
  }

  auto reader = PcapReader::open(path);
  ASSERT_TRUE(reader.has_value());
  const auto dgram = reader->next();
  ASSERT_TRUE(dgram.has_value());
  EXPECT_TRUE(dgram->payload.empty());
}

TEST(Pcap, RejectsAFileThatIsNotACapture) {
  const auto path = temp_path("garbage");
  {
    auto writer = PcapWriter::create(path);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->close().has_value());
  }
  // Overwrite the magic.
  std::FILE* f = std::fopen(path.c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  const char junk[4] = {'j', 'u', 'n', 'k'};
  ASSERT_EQ(std::fwrite(junk, 1, 4, f), 4U);
  std::fclose(f);

  const auto reader = PcapReader::open(path);
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error(), PcapError::BadMagic);
}

TEST(Pcap, MissingFileIsAnIoError) {
  const auto reader = PcapReader::open("/nonexistent/definitely/not/here.pcap");
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error(), PcapError::Io);
}

TEST(Pcap, RewindReplaysTheSameCapture) {
  const auto path = temp_path("rewind");
  {
    auto writer = PcapWriter::create(path);
    ASSERT_TRUE(writer.has_value());
    for (std::uint8_t i = 0; i < 5; ++i) {
      const auto payload = bytes_of({i, static_cast<std::uint8_t>(i + 1)});
      ASSERT_TRUE(writer->write_udp(payload, 1000 + i, ipv4(192, 0, 2, 10), 1, kGroupIncrA,
                                    kPortIncrA)
                      .has_value());
    }
    ASSERT_TRUE(writer->close().has_value());
  }

  auto reader = PcapReader::open(path);
  ASSERT_TRUE(reader.has_value());

  const auto drain = [&reader] {
    std::vector<std::uint8_t> firsts;
    while (const auto d = reader->next()) {
      firsts.push_back(std::to_integer<std::uint8_t>(d->payload[0]));
    }
    return firsts;
  };

  const auto pass1 = drain();
  reader->rewind();
  const auto pass2 = drain();

  EXPECT_EQ(pass1, (std::vector<std::uint8_t>{0, 1, 2, 3, 4}));
  EXPECT_EQ(pass1, pass2) << "replay must be deterministic";
}

// ─── Transport demultiplexing ────────────────────────────────────────

TEST(PcapTransport, DemuxesChannelsAndLinesByDestination) {
  const auto path = temp_path("demux");
  {
    auto writer = PcapWriter::create(path);
    ASSERT_TRUE(writer.has_value());
    const auto payload = bytes_of({0x01});
    const auto src     = ipv4(192, 0, 2, 10);
    ASSERT_TRUE(writer->write_udp(payload, 1, src, 1, kGroupIncrA, kPortIncrA).has_value());
    ASSERT_TRUE(writer->write_udp(payload, 2, src, 1, kGroupIncrB, kPortIncrB).has_value());
    ASSERT_TRUE(writer->write_udp(payload, 3, src, 1, kGroupSnapA, kPortSnapA).has_value());
    // Unrelated traffic, as any real capture contains.
    ASSERT_TRUE(writer->write_udp(payload, 4, src, 1, ipv4(239, 9, 9, 9), 9999).has_value());
    ASSERT_TRUE(writer->close().has_value());
  }

  auto transport = PcapTransport::open(path, ref_config());
  ASSERT_TRUE(transport.has_value()) << to_string(transport.error());

  const auto first = transport->poll();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->kind, ChannelKind::Incremental);
  EXPECT_EQ(first->line, LineId::A);

  const auto second = transport->poll();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->kind, ChannelKind::Incremental);
  EXPECT_EQ(second->line, LineId::B);

  const auto third = transport->poll();
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third->kind, ChannelKind::Snapshot);
  EXPECT_EQ(third->line, LineId::A);

  EXPECT_FALSE(transport->poll().has_value());
  EXPECT_TRUE(transport->exhausted());
  EXPECT_EQ(transport->delivered(), 3U);
  EXPECT_EQ(transport->unmatched(), 1U) << "foreign traffic is skipped, not fatal";
}

TEST(ChannelConfigLookup, ResolvesEndpointsAndKinds) {
  const auto cfg = ref_config();

  const auto incr_a = cfg.endpoint_for(ChannelKind::Incremental, LineId::A);
  ASSERT_TRUE(incr_a.has_value());
  EXPECT_EQ(incr_a->port, kPortIncrA);
  EXPECT_TRUE(incr_a->valid());

  EXPECT_FALSE(cfg.endpoint_for(ChannelKind::Snapshot, LineId::B).has_value());
  EXPECT_TRUE(cfg.has(ChannelKind::Snapshot));
  EXPECT_FALSE(cfg.has(ChannelKind::HistoricalReplay));
  EXPECT_FALSE(cfg.classify(ipv4(1, 2, 3, 4), 1).has_value());
}

TEST(Ipv4Helper, FormatsDottedQuad) {
  EXPECT_EQ(ipv4_to_string(ipv4(224, 0, 1, 1)), "224.0.1.1");
  EXPECT_EQ(ipv4_to_string(0), "0.0.0.0");
  EXPECT_EQ(ipv4_to_string(0xFFFFFFFFU), "255.255.255.255");
}

}  // namespace
