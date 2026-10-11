#include "rfeed/packet.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rfeed/sbe_header.hpp"
#include "rfeed/types.hpp"
#include "wire_builder.hpp"

namespace {

using rfeed::DecodeError;
using rfeed::MessageFramer;
using rfeed::PacketView;
using rfeed::SbeMessageHeader;
using rfeed::test::append;
using rfeed::test::MsgSpec;
using rfeed::test::packet;
using rfeed::test::well_formed;
using rfeed::test::WireBuilder;

constexpr std::uint32_t kSeq          = 0x0A0B0C0DU;
constexpr std::uint64_t kSendingTime  = 1'700'000'000'123'456'789ULL;
constexpr std::uint16_t kTemplateBook = 46;

std::vector<std::uint8_t> to_octets(std::span<const std::byte> s) {
  std::vector<std::uint8_t> out;
  out.reserve(s.size());
  for (const auto b : s) out.push_back(std::to_integer<std::uint8_t>(b));
  return out;
}

// ─── Packet header ───────────────────────────────────────────────────

TEST(PacketHeader, DecodesSequenceAndSendingTime) {
  auto b = packet(kSeq, kSendingTime);
  ASSERT_EQ(b.size(), rfeed::PacketHeader::kEncodedSize);

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  EXPECT_EQ(pkt->header.seq, kSeq);
  EXPECT_EQ(pkt->header.sending_time, static_cast<rfeed::Timestamp>(kSendingTime));
}

TEST(PacketHeader, RejectsDatagramShorterThanTwelveBytes) {
  auto b = packet(kSeq, kSendingTime);
  for (std::size_t n = 0; n < rfeed::PacketHeader::kEncodedSize; ++n) {
    const auto pkt = PacketView::decode(b.view().first(n));
    ASSERT_FALSE(pkt.has_value()) << "accepted a " << n << "-byte datagram";
    EXPECT_EQ(pkt.error(), DecodeError::Truncated);
  }
}

TEST(PacketHeader, HeaderOnlyDatagramIsAHeartbeat) {
  auto b = packet(kSeq, kSendingTime);

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  EXPECT_TRUE(pkt->is_heartbeat());

  auto framer = pkt->framer();
  EXPECT_EQ(framer.next().error(), DecodeError::EndOfPacket);
  EXPECT_FALSE(framer.failed());
}

// ─── Well-formed framing ─────────────────────────────────────────────

TEST(MessageFramer, DecodesASingleMessage) {
  auto b = packet(kSeq, kSendingTime);
  append(b, well_formed(kTemplateBook, {0xDE, 0xAD, 0xBE, 0xEF}));

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  EXPECT_FALSE(pkt->is_heartbeat());

  auto framer = pkt->framer();
  const auto msg = framer.next();
  ASSERT_TRUE(msg.has_value()) << rfeed::to_string(msg.error());
  EXPECT_EQ(msg->hdr.template_id, kTemplateBook);
  EXPECT_EQ(msg->hdr.schema_id, 1U);
  EXPECT_EQ(msg->hdr.version, 13U);
  EXPECT_EQ(msg->hdr.block_length, 4U);
  EXPECT_EQ(to_octets(msg->body), (std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));

  EXPECT_EQ(framer.next().error(), DecodeError::EndOfPacket);
}

TEST(MessageFramer, DecodesMultipleMessagesInOneDatagram) {
  auto b = packet(kSeq, kSendingTime);
  append(b, well_formed(30, {0x01, 0x02}));
  append(b, well_formed(kTemplateBook, {0x03, 0x04, 0x05, 0x06, 0x07}));
  append(b, well_formed(52, {}));

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  const auto first = framer.next();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->hdr.template_id, 30U);
  EXPECT_EQ(first->body.size(), 2U);

  const auto second = framer.next();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->hdr.template_id, kTemplateBook);
  EXPECT_EQ(second->body.size(), 5U);

  const auto third = framer.next();
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third->hdr.template_id, 52U);
  EXPECT_TRUE(third->body.empty());

  EXPECT_EQ(framer.next().error(), DecodeError::EndOfPacket);
  EXPECT_FALSE(framer.failed());
  EXPECT_EQ(framer.bytes_consumed(), pkt->messages.size());
}

TEST(MessageFramer, BodyMayBeLargerThanBlockLengthWhenGroupsFollow) {
  // Root block is 4 bytes; the trailing 6 are a repeating group the generated
  // codec will walk. Framing must hand over the whole span, not truncate it.
  MsgSpec m{};
  m.body         = {0x01, 0x02, 0x03, 0x04, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
  m.block_length = 4;
  m.template_id  = kTemplateBook;
  m.msg_size     = static_cast<std::uint16_t>(SbeMessageHeader::kEncodedSize + m.body.size());

  auto b = packet(kSeq, kSendingTime);
  append(b, m);

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  const auto msg = framer.next();
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->hdr.block_length, 4U);
  EXPECT_EQ(msg->body.size(), 10U);
}

// ─── Malformed framing ───────────────────────────────────────────────
//
// None of these may read out of bounds, and none may fail to terminate.

TEST(MessageFramer, ZeroMessageSizeIsRejectedAndLatches) {
  MsgSpec m{};
  m.msg_size    = 0;  // would advance the cursor by nothing and spin forever
  m.template_id = kTemplateBook;
  m.body        = {0x01, 0x02, 0x03, 0x04};

  auto b = packet(kSeq, kSendingTime);
  append(b, m);

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  EXPECT_EQ(framer.next().error(), DecodeError::BadMessageSize);
  EXPECT_TRUE(framer.failed());
  // Latched: the same error forever, never EndOfPacket, never progress.
  EXPECT_EQ(framer.next().error(), DecodeError::BadMessageSize);
  EXPECT_EQ(framer.next().error(), DecodeError::BadMessageSize);
}

TEST(MessageFramer, MessageSizeBelowSbeHeaderIsRejected) {
  for (std::uint16_t size = 1; size < SbeMessageHeader::kEncodedSize; ++size) {
    MsgSpec m{};
    m.msg_size    = size;
    m.template_id = kTemplateBook;
    m.body        = {0x01, 0x02, 0x03, 0x04};

    auto b = packet(kSeq, kSendingTime);
    append(b, m);

    const auto pkt = PacketView::decode(b.view());
    ASSERT_TRUE(pkt.has_value());
    auto framer = pkt->framer();
    EXPECT_EQ(framer.next().error(), DecodeError::BadMessageSize) << "MsgSize=" << size;
  }
}

TEST(MessageFramer, MessageSizeOverrunningTheDatagramIsRejected) {
  MsgSpec m = well_formed(kTemplateBook, {0x01, 0x02, 0x03, 0x04});
  m.msg_size = static_cast<std::uint16_t>(m.msg_size + 1);  // one byte too greedy

  auto b = packet(kSeq, kSendingTime);
  append(b, m);

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  EXPECT_EQ(framer.next().error(), DecodeError::MessageOverrunsPacket);
  EXPECT_TRUE(framer.failed());
}

TEST(MessageFramer, WildlyOversizedMessageSizeIsRejected) {
  MsgSpec m = well_formed(kTemplateBook, {0x01, 0x02, 0x03, 0x04});
  m.msg_size = 0xFFFF;

  auto b = packet(kSeq, kSendingTime);
  append(b, m);

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  EXPECT_EQ(pkt->framer().next().error(), DecodeError::MessageOverrunsPacket);
}

TEST(MessageFramer, BlockLengthOverrunningTheMessageIsRejected) {
  MsgSpec m     = well_formed(kTemplateBook, {0x01, 0x02, 0x03, 0x04});
  m.block_length = 64;  // claims a root block far larger than the framed message

  auto b = packet(kSeq, kSendingTime);
  append(b, m);

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  EXPECT_EQ(framer.next().error(), DecodeError::BlockLengthOverrunsMessage);
  EXPECT_TRUE(framer.failed());
}

TEST(MessageFramer, TrailingOctetAfterLastMessageIsAFramingFault) {
  auto b = packet(kSeq, kSendingTime);
  append(b, well_formed(kTemplateBook, {0x01, 0x02}));
  b.fill(1, 0xFF);  // one stray byte: not enough for another MsgSize

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  ASSERT_TRUE(framer.next().has_value());
  EXPECT_EQ(framer.next().error(), DecodeError::Truncated);
  EXPECT_TRUE(framer.failed());
}

TEST(MessageFramer, FaultAbandonsTheRestOfThePacket) {
  MsgSpec bad = well_formed(kTemplateBook, {0x01, 0x02});
  bad.msg_size = 3;  // below the SBE header size

  auto b = packet(kSeq, kSendingTime);
  append(b, well_formed(30, {0xAA}));
  append(b, bad);
  append(b, well_formed(52, {0xBB}));

  const auto pkt = PacketView::decode(b.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  const auto first = framer.next();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->hdr.template_id, 30U);

  // Once framing desynchronises, everything after it is untrustworthy — the
  // third message is NOT recovered even though it is well-formed in isolation.
  EXPECT_EQ(framer.next().error(), DecodeError::BadMessageSize);
  EXPECT_EQ(framer.next().error(), DecodeError::BadMessageSize);
}

// ─── Truncation sweep ────────────────────────────────────────────────

TEST(MessageFramer, EveryPrefixOfAValidPacketTerminatesSafely) {
  auto b = packet(kSeq, kSendingTime);
  append(b, well_formed(30, {0x01, 0x02}));
  append(b, well_formed(kTemplateBook, {0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
  append(b, well_formed(52, {0x09}));
  const auto full = b.view();

  // Datagrams get truncated in the wild. Under ASan/UBSan this is also the
  // out-of-bounds check for every code path in the framer.
  for (std::size_t n = 0; n <= full.size(); ++n) {
    const auto pkt = PacketView::decode(full.first(n));
    if (!pkt.has_value()) {
      ASSERT_EQ(pkt.error(), DecodeError::Truncated) << "prefix " << n;
      continue;
    }

    auto        framer     = pkt->framer();
    std::size_t iterations = 0;
    while (framer.next().has_value()) {
      ++iterations;
      ASSERT_LT(iterations, 16U) << "framer failed to terminate at prefix " << n;
    }
  }
}

}  // namespace
