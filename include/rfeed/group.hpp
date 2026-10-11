#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "rfeed/byte_reader.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

// Cursor over an SBE repeating group.
//
// The group is introduced by a `groupSize` composite: uint16 blockLength, then
// numInGroup — one byte on CME, two elsewhere. That width is a venue policy
// parameter, not a constant, because getting it wrong misparses every group
// after the first with no error raised.
//
// Each entry is handed back as a ByteReader bounded to exactly blockLength
// bytes, so trailing padding inside an entry is skipped automatically and a
// decoder physically cannot read into the next entry.
// Seek to the end of a root block before reading its repeating group.
//
// SBE places the group at exactly blockLength from the start of the block, not
// immediately after the last field a decoder happens to read. Assuming the two
// coincide works right up until the schema gains trailing padding or an
// unhandled trailing field, and then misparses every group.
[[nodiscard]] inline std::expected<void, DecodeError> seek_past_block(
    ByteReader& in, std::uint16_t block_length) noexcept {
  if (in.offset() > block_length) return std::unexpected{DecodeError::BlockLengthOverrunsMessage};
  return in.skip(static_cast<std::size_t>(block_length) - in.offset());
}

template <std::size_t CountBytes>
  requires(CountBytes == 1 || CountBytes == 2)
class GroupReader {
 public:
  GroupReader() noexcept = default;

  // Consumes the group header from `in`, then takes ownership of the entry
  // region so the caller's reader is positioned after the whole group.
  [[nodiscard]] static std::expected<GroupReader, DecodeError> decode(ByteReader& in) noexcept {
    const auto block_length = in.read<std::uint16_t>();
    if (!block_length) return std::unexpected{DecodeError::BadGroupHeader};

    std::uint32_t count = 0;
    if constexpr (CountBytes == 1) {
      const auto n = in.read<std::uint8_t>();
      if (!n) return std::unexpected{DecodeError::BadGroupHeader};
      count = *n;
    } else {
      const auto n = in.read<std::uint16_t>();
      if (!n) return std::unexpected{DecodeError::BadGroupHeader};
      count = *n;
    }

    // Overflow-safe: both operands widen to 64 bits before multiplying.
    const auto span_bytes =
        static_cast<std::uint64_t>(*block_length) * static_cast<std::uint64_t>(count);
    if (span_bytes > in.remaining()) return std::unexpected{DecodeError::GroupOverrunsMessage};

    const auto entries = in.read_bytes(static_cast<std::size_t>(span_bytes));
    if (!entries) return std::unexpected{entries.error()};

    GroupReader g;
    g.entries_      = ByteReader{*entries};
    g.block_length_ = *block_length;
    g.remaining_    = count;
    return g;
  }

  // Next entry, or DecodeError::EndOfGroup when exhausted.
  [[nodiscard]] std::expected<ByteReader, DecodeError> next() noexcept {
    if (remaining_ == 0) return std::unexpected{DecodeError::EndOfGroup};
    const auto entry = entries_.read_bytes(block_length_);
    if (!entry) return std::unexpected{entry.error()};
    --remaining_;
    return ByteReader{*entry};
  }

  [[nodiscard]] constexpr std::uint16_t block_length() const noexcept { return block_length_; }
  [[nodiscard]] constexpr std::uint32_t remaining() const noexcept { return remaining_; }

 private:
  ByteReader    entries_{};
  std::uint16_t block_length_{0};
  std::uint32_t remaining_{0};
};

}  // namespace rfeed
