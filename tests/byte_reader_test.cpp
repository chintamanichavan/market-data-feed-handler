#include "rfeed/byte_reader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rfeed/types.hpp"
#include "wire_builder.hpp"

namespace {

using rfeed::ByteReader;
using rfeed::DecodeError;
using rfeed::test::WireBuilder;

// ─── Constant evaluation ─────────────────────────────────────────────
// The reader is constexpr so golden vectors can be checked at compile time.

constexpr std::array<std::byte, 2> kLe16{std::byte{0x34}, std::byte{0x12}};

constexpr std::uint16_t read_le16() {
  ByteReader r{std::span<const std::byte>{kLe16}};
  return *r.read<std::uint16_t>();
}

static_assert(read_le16() == 0x1234U);

// ─── Endianness ──────────────────────────────────────────────────────

TEST(ByteReader, ReadsLittleEndianScalarsInSequence) {
  const std::array<std::byte, 8> bytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                       std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
                                       std::byte{0x07}, std::byte{0x08}};
  ByteReader r{std::span<const std::byte>{bytes}};

  EXPECT_EQ(r.read<std::uint16_t>().value(), 0x0201U);
  EXPECT_EQ(r.read<std::uint32_t>().value(), 0x06050403U);
  EXPECT_EQ(r.read<std::uint16_t>().value(), 0x0807U);
  EXPECT_TRUE(r.empty());
}

TEST(ByteReader, ReadsSixtyFourBitValues) {
  WireBuilder b;
  b.le(std::uint64_t{0x0102030405060708ULL});
  ByteReader r{b.view()};

  EXPECT_EQ(r.read<std::uint64_t>().value(), 0x0102030405060708ULL);
}

TEST(ByteReader, PreservesSignedness) {
  WireBuilder b;
  b.le(std::int16_t{-1}).le(std::int32_t{-2}).le(std::int64_t{-3});
  ByteReader r{b.view()};

  EXPECT_EQ(r.read<std::int16_t>().value(), -1);
  EXPECT_EQ(r.read<std::int32_t>().value(), -2);
  EXPECT_EQ(r.read<std::int64_t>().value(), -3);
}

TEST(ByteReader, ReadsSingleOctets) {
  WireBuilder b;
  b.raw({0xFF, 0x00, 0x7F});
  ByteReader r{b.view()};

  EXPECT_EQ(r.read<std::uint8_t>().value(), 0xFFU);
  EXPECT_EQ(r.read<std::uint8_t>().value(), 0x00U);
  EXPECT_EQ(r.read<std::int8_t>().value(), 0x7F);
}

// ─── Bounds ──────────────────────────────────────────────────────────

TEST(ByteReader, FailedReadDoesNotAdvanceCursor) {
  WireBuilder b;
  b.raw({0xAA, 0xBB, 0xCC});
  ByteReader r{b.view()};

  const auto too_big = r.read<std::uint32_t>();
  ASSERT_FALSE(too_big.has_value());
  EXPECT_EQ(too_big.error(), DecodeError::Truncated);
  EXPECT_EQ(r.offset(), 0U);
  EXPECT_EQ(r.remaining(), 3U);

  // Still usable: the reader was not left torn.
  EXPECT_EQ(r.read<std::uint16_t>().value(), 0xBBAAU);
  EXPECT_EQ(r.remaining(), 1U);
}

TEST(ByteReader, EmptySpanFailsEveryRead) {
  ByteReader r{std::span<const std::byte>{}};

  EXPECT_TRUE(r.empty());
  EXPECT_EQ(r.remaining(), 0U);
  EXPECT_EQ(r.read<std::uint8_t>().error(), DecodeError::Truncated);
  EXPECT_EQ(r.read_bytes(1).error(), DecodeError::Truncated);
  EXPECT_EQ(r.skip(1).error(), DecodeError::Truncated);
  EXPECT_TRUE(r.rest().empty());
}

TEST(ByteReader, DefaultConstructedIsEmpty) {
  ByteReader r;
  EXPECT_TRUE(r.empty());
  EXPECT_EQ(r.offset(), 0U);
  EXPECT_EQ(r.read<std::uint8_t>().error(), DecodeError::Truncated);
}

TEST(ByteReader, ReadBytesReturnsExactSubspan) {
  WireBuilder b;
  b.raw({0x01, 0x02, 0x03, 0x04, 0x05});
  ByteReader r{b.view()};

  ASSERT_TRUE(r.skip(1).has_value());
  const auto chunk = r.read_bytes(3);
  ASSERT_TRUE(chunk.has_value());
  ASSERT_EQ(chunk->size(), 3U);
  EXPECT_EQ(std::to_integer<std::uint8_t>((*chunk)[0]), 0x02U);
  EXPECT_EQ(std::to_integer<std::uint8_t>((*chunk)[2]), 0x04U);
  EXPECT_EQ(r.remaining(), 1U);
}

TEST(ByteReader, ZeroLengthReadAndSkipSucceedAtEnd) {
  WireBuilder b;
  b.raw({0x01});
  ByteReader r{b.view()};
  ASSERT_TRUE(r.skip(1).has_value());

  EXPECT_TRUE(r.read_bytes(0).has_value());
  EXPECT_TRUE(r.skip(0).has_value());
  EXPECT_TRUE(r.empty());
}

TEST(ByteReader, OversizedReadBytesIsRejectedWithoutAdvancing) {
  WireBuilder b;
  b.raw({0x01, 0x02});
  ByteReader r{b.view()};

  EXPECT_EQ(r.read_bytes(3).error(), DecodeError::Truncated);
  EXPECT_EQ(r.offset(), 0U);
  EXPECT_EQ(r.skip(3).error(), DecodeError::Truncated);
  EXPECT_EQ(r.offset(), 0U);
}

TEST(ByteReader, RestDoesNotAdvance) {
  WireBuilder b;
  b.raw({0x01, 0x02, 0x03});
  ByteReader r{b.view()};
  ASSERT_TRUE(r.skip(1).has_value());

  EXPECT_EQ(r.rest().size(), 2U);
  EXPECT_EQ(r.rest().size(), 2U);
  EXPECT_EQ(r.offset(), 1U);
}

}  // namespace
