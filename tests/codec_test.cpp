#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ref_encoder.hpp"
#include "ref_venue.hpp"
#include "rfeed/group.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/packet.hpp"
#include "rfeed/venue.hpp"

namespace {

using namespace rfeed;
using rfeed::venue_ref::Encoder;
using rfeed::venue_ref::RefCodec;
using rfeed::venue_ref::RefVenue;

using RefGroup = GroupReader<RefVenue::kGroupCountBytes>;

// Encode → frame → decode. A layout misreading has to be made twice, in the
// same direction, to survive this — which is exactly the property a
// hand-written codec needs and a generated one gets for free.

MessageView first_message(const Encoder& enc, MessageFramer& framer_out) {
  const auto pkt = PacketView::decode(enc.view());
  EXPECT_TRUE(pkt.has_value());
  framer_out = pkt->framer();
  const auto msg = framer_out.next();
  EXPECT_TRUE(msg.has_value());
  return msg.has_value() ? *msg : MessageView{};
}

TEST(RefCodec, IncrementalBookRoundTrips) {
  const std::vector<MdEntry> entries{
      MdEntry{.px          = Mantissa{4'275'250'000'000},
              .size        = 37,
              .security    = 1234,
              .rpt_seq     = 900,
              .num_orders  = 5,
              .price_level = 1,
              .action      = UpdateAction::New,
              .type        = EntryType::Bid},
      MdEntry{.px          = Mantissa{-500'000'000},  // negative prices are legal
              .size        = 0,
              .security    = 1234,
              .rpt_seq     = 901,
              .num_orders  = 0,
              .price_level = 3,
              .action      = UpdateAction::Delete,
              .type        = EntryType::ImpliedOffer},
  };
  const IncrementalHeader header{.transact_time = 1'700'000'000'000'000'001, .end_of_event = true};

  Encoder enc{42, 999};
  enc.incremental(header, entries);

  MessageFramer framer;
  const auto    msg = first_message(enc, framer);
  ASSERT_EQ(RefVenue::classify(msg.hdr.template_id), LogicalMessage::IncrementalBook);
  ASSERT_EQ(msg.hdr.schema_id, RefVenue::kSchemaId);

  ByteReader in{msg.body};
  const auto decoded_header = RefCodec::incremental_header(in);
  ASSERT_TRUE(decoded_header.has_value());
  EXPECT_EQ(decoded_header->transact_time, header.transact_time);
  EXPECT_TRUE(decoded_header->end_of_event);

  ASSERT_TRUE(seek_past_block(in, msg.hdr.block_length).has_value());
  auto group = RefGroup::decode(in);
  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(group->remaining(), 2U);
  EXPECT_EQ(group->block_length(), venue_ref::kBlkMdEntry);

  for (const auto& expected : entries) {
    auto entry_reader = group->next();
    ASSERT_TRUE(entry_reader.has_value());
    const auto entry = RefCodec::md_entry(*entry_reader);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->px.raw, expected.px.raw);
    EXPECT_EQ(entry->size, expected.size);
    EXPECT_EQ(entry->security, expected.security);
    EXPECT_EQ(entry->rpt_seq, expected.rpt_seq);
    EXPECT_EQ(entry->num_orders, expected.num_orders);
    EXPECT_EQ(entry->price_level, expected.price_level);
    EXPECT_EQ(entry->action, expected.action);
    EXPECT_EQ(entry->type, expected.type);
  }

  EXPECT_EQ(group->next().error(), DecodeError::EndOfGroup);
  EXPECT_EQ(framer.next().error(), DecodeError::EndOfPacket);
}

TEST(RefCodec, SnapshotRoundTripsIncludingRootPadding) {
  // The snapshot root block has one padding byte after TradingStatus. If the
  // decoder walked straight from the last field into the group instead of
  // seeking to blockLength, the group header would be read one byte early.
  const std::vector<SnapshotEntry> entries{
      SnapshotEntry{Mantissa{100}, 10, 2, 1, EntryType::Bid},
      SnapshotEntry{Mantissa{101}, 20, 3, 1, EntryType::Offer},
      SnapshotEntry{Mantissa{99}, 30, 4, 2, EntryType::Bid},
  };
  const SnapshotHeader header{.last_msg_seq_num_processed = 555,
                              .tot_num_reports            = 8,
                              .security                   = 77,
                              .rpt_seq                    = 4321,
                              .transact_time              = 123'456'789,
                              .status                     = TradingStatus::Open};

  Encoder enc{1, 0};
  enc.snapshot(header, entries);

  MessageFramer framer;
  const auto    msg = first_message(enc, framer);
  ASSERT_EQ(RefVenue::classify(msg.hdr.template_id), LogicalMessage::SnapshotFullRefresh);
  EXPECT_EQ(msg.hdr.block_length, venue_ref::kBlkSnapshot);

  ByteReader in{msg.body};
  const auto decoded = RefCodec::snapshot_header(in);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->last_msg_seq_num_processed, 555U);
  EXPECT_EQ(decoded->tot_num_reports, 8U);
  EXPECT_EQ(decoded->security, 77);
  EXPECT_EQ(decoded->rpt_seq, 4321U);
  EXPECT_EQ(decoded->status, TradingStatus::Open);
  EXPECT_LT(in.offset(), venue_ref::kBlkSnapshot) << "root block should have trailing padding";

  ASSERT_TRUE(seek_past_block(in, msg.hdr.block_length).has_value());
  auto group = RefGroup::decode(in);
  ASSERT_TRUE(group.has_value());
  ASSERT_EQ(group->remaining(), 3U);

  for (const auto& expected : entries) {
    auto entry_reader = group->next();
    ASSERT_TRUE(entry_reader.has_value());
    const auto entry = RefCodec::snapshot_entry(*entry_reader);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->px.raw, expected.px.raw);
    EXPECT_EQ(entry->size, expected.size);
    EXPECT_EQ(entry->num_orders, expected.num_orders);
    EXPECT_EQ(entry->price_level, expected.price_level);
    EXPECT_EQ(entry->type, expected.type);
  }
}

TEST(RefCodec, InstrumentDefinitionRoundTrips) {
  InstrumentDefinition def{};
  def.security            = 9876;
  const std::string_view symbol = "ESZ6";
  for (std::size_t i = 0; i < symbol.size(); ++i) def.symbol[i] = symbol[i];
  def.min_price_increment = Mantissa{25'000'000};
  def.tot_num_reports     = 3;
  def.market_depth        = 10;
  def.sub_tick_divisor    = 2;

  Encoder enc{5, 0};
  enc.instrument_definition(def);

  MessageFramer framer;
  const auto    msg = first_message(enc, framer);
  ASSERT_EQ(RefVenue::classify(msg.hdr.template_id), LogicalMessage::InstrumentDefinition);

  ByteReader in{msg.body};
  const auto decoded = RefCodec::instrument_definition(in);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->security, 9876);
  EXPECT_EQ(decoded->symbol_view(), "ESZ6");
  EXPECT_EQ(decoded->min_price_increment.raw, 25'000'000);
  EXPECT_EQ(decoded->market_depth, 10U);
  EXPECT_EQ(decoded->sub_tick_divisor, 2U);
}

TEST(RefCodec, ZeroSubTickDivisorIsClampedToOne) {
  // A zero on the wire would make every tick conversion divide by zero.
  InstrumentDefinition def{};
  def.security         = 1;
  def.sub_tick_divisor = 0;

  Encoder enc{1, 0};
  enc.instrument_definition(def);

  MessageFramer framer;
  const auto    msg = first_message(enc, framer);
  ByteReader    in{msg.body};
  const auto    decoded = RefCodec::instrument_definition(in);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->sub_tick_divisor, 1U);
}

TEST(RefCodec, StatusAndResetRoundTrip) {
  Encoder enc{3, 0};
  enc.security_status(SecurityStatusMsg{.security = 11, .transact_time = 7, .status = TradingStatus::Halted});
  enc.channel_reset(ChannelResetMsg{.transact_time = 8, .kind = ResetKind::PeriodStart});
  enc.admin_heartbeat();
  ASSERT_EQ(enc.message_count(), 3U);

  const auto pkt = PacketView::decode(enc.view());
  ASSERT_TRUE(pkt.has_value());
  auto framer = pkt->framer();

  const auto status_msg = framer.next();
  ASSERT_TRUE(status_msg.has_value());
  ASSERT_EQ(RefVenue::classify(status_msg->hdr.template_id), LogicalMessage::SecurityStatus);
  ByteReader status_in{status_msg->body};
  const auto status = RefCodec::security_status(status_in);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->security, 11);
  EXPECT_EQ(status->status, TradingStatus::Halted);

  const auto reset_msg = framer.next();
  ASSERT_TRUE(reset_msg.has_value());
  ASSERT_EQ(RefVenue::classify(reset_msg->hdr.template_id), LogicalMessage::ChannelReset);
  ByteReader reset_in{reset_msg->body};
  const auto reset = RefCodec::channel_reset(reset_in);
  ASSERT_TRUE(reset.has_value());
  EXPECT_EQ(reset->kind, ResetKind::PeriodStart);

  const auto hb = framer.next();
  ASSERT_TRUE(hb.has_value());
  EXPECT_EQ(RefVenue::classify(hb->hdr.template_id), LogicalMessage::Heartbeat);
  EXPECT_EQ(hb->hdr.block_length, 0U);

  EXPECT_EQ(framer.next().error(), DecodeError::EndOfPacket);
}

TEST(RefVenuePolicy, UnknownTemplateIdsClassifyAsUnknown) {
  EXPECT_EQ(RefVenue::classify(0), LogicalMessage::Unknown);
  EXPECT_EQ(RefVenue::classify(999), LogicalMessage::Unknown);
  EXPECT_EQ(RefVenue::classify(47), LogicalMessage::Unknown);
}

TEST(RefVenuePolicy, PeriodStartResetClearsMoreThanIntraday) {
  const auto intraday = RefVenue::reset_scope(ResetKind::Intraday);
  EXPECT_TRUE(intraday.books);
  EXPECT_TRUE(intraday.sequence);
  EXPECT_FALSE(intraday.definitions);
  EXPECT_FALSE(intraday.security_id_namespace);

  const auto period = RefVenue::reset_scope(ResetKind::PeriodStart);
  EXPECT_TRUE(period.definitions);
  EXPECT_TRUE(period.security_id_namespace);
}

// ─── Group decoding under malformed input ────────────────────────────

TEST(GroupReader, RejectsCountThatOverrunsTheMessage) {
  // blockLength 32, numInGroup 200 => 6400 bytes claimed from a short message.
  std::vector<std::byte> body;
  const auto             push = [&body](std::uint8_t v) { body.push_back(std::byte{v}); };
  push(32); push(0);  // blockLength = 32
  push(200);          // numInGroup
  for (int i = 0; i < 16; ++i) push(0xAA);

  ByteReader in{body};
  const auto group = RefGroup::decode(in);
  ASSERT_FALSE(group.has_value());
  EXPECT_EQ(group.error(), DecodeError::GroupOverrunsMessage);
}

TEST(GroupReader, RejectsTruncatedGroupHeader) {
  std::vector<std::byte> body{std::byte{32}, std::byte{0}};  // no numInGroup
  ByteReader             in{body};
  EXPECT_EQ(RefGroup::decode(in).error(), DecodeError::BadGroupHeader);
}

TEST(GroupReader, EmptyGroupIsLegal) {
  std::vector<std::byte> body{std::byte{32}, std::byte{0}, std::byte{0}};
  ByteReader             in{body};
  auto                   group = RefGroup::decode(in);
  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(group->remaining(), 0U);
  EXPECT_EQ(group->next().error(), DecodeError::EndOfGroup);
}

TEST(GroupReader, EntryReaderCannotStrayIntoTheNextEntry) {
  // Two 4-byte entries. Reading a fifth byte from the first must fail rather
  // than silently consume the second entry's first byte.
  std::vector<std::byte> body{std::byte{4}, std::byte{0}, std::byte{2}};
  for (std::uint8_t i = 0; i < 8; ++i) body.push_back(std::byte{i});

  ByteReader in{body};
  auto       group = RefGroup::decode(in);
  ASSERT_TRUE(group.has_value());

  auto first = group->next();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->read<std::uint32_t>().value(), 0x03020100U);
  EXPECT_EQ(first->read<std::uint8_t>().error(), DecodeError::Truncated);

  auto second = group->next();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->read<std::uint32_t>().value(), 0x07060504U);
}

TEST(GroupReader, WiderCountFieldChangesTheHeaderLayout) {
  // The single byte that separates two "identical" MDP 3.0 venues. Same bytes,
  // different numInGroup width, completely different parse.
  std::vector<std::byte> body{std::byte{4}, std::byte{0}, std::byte{2}, std::byte{0}};
  for (std::uint8_t i = 0; i < 8; ++i) body.push_back(std::byte{i});

  ByteReader narrow_in{body};
  auto       narrow = GroupReader<1>::decode(narrow_in);
  ASSERT_TRUE(narrow.has_value());
  EXPECT_EQ(narrow->remaining(), 2U);

  ByteReader wide_in{body};
  auto       wide = GroupReader<2>::decode(wide_in);
  ASSERT_TRUE(wide.has_value());
  EXPECT_EQ(wide->remaining(), 2U);

  // Same count, but the wide reader consumed one more header byte, so every
  // entry after it is shifted. Silent corruption if the width is guessed.
  auto narrow_entry = narrow->next();
  auto wide_entry   = wide->next();
  ASSERT_TRUE(narrow_entry.has_value());
  ASSERT_TRUE(wide_entry.has_value());
  EXPECT_EQ(narrow_entry->read<std::uint32_t>().value(), 0x02010000U);
  EXPECT_EQ(wide_entry->read<std::uint32_t>().value(), 0x03020100U);
}

}  // namespace
