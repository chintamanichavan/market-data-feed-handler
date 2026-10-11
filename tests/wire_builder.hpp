#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <vector>

#include "rfeed/byte_reader.hpp"
#include "rfeed/sbe_header.hpp"

namespace rfeed::test {

// Little-endian byte assembler for hand-authored golden vectors.
//
// Deliberately dumb and independent of ByteReader: a test fixture that shares
// the production encoder's assumptions cannot catch the production encoder
// being wrong.
class WireBuilder {
 public:
  template <WireInt T>
  WireBuilder& le(T value) {
    using U     = std::make_unsigned_t<T>;
    const auto u = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((u >> (8U * i)) & 0xFFU)));
    }
    return *this;
  }

  WireBuilder& raw(std::initializer_list<std::uint8_t> octets) {
    for (const auto o : octets) bytes_.push_back(static_cast<std::byte>(o));
    return *this;
  }

  WireBuilder& raw(std::span<const std::uint8_t> octets) {
    for (const auto o : octets) bytes_.push_back(static_cast<std::byte>(o));
    return *this;
  }

  WireBuilder& fill(std::size_t n, std::uint8_t octet = 0) {
    for (std::size_t i = 0; i < n; ++i) bytes_.push_back(static_cast<std::byte>(octet));
    return *this;
  }

  [[nodiscard]] std::span<const std::byte> view() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

 private:
  std::vector<std::byte> bytes_{};
};

// One framed message. Every field is written verbatim so a test can make any of
// them deliberately wrong — that is the point of the malformed-input suite.
struct MsgSpec {
  std::uint16_t             msg_size{};      // written as-is, correct or not
  std::uint16_t             block_length{};
  std::uint16_t             template_id{};
  std::uint16_t             schema_id{1};
  std::uint16_t             version{13};
  std::vector<std::uint8_t> body{};
};

// A message whose MsgSize is consistent with its contents.
inline MsgSpec well_formed(std::uint16_t template_id, std::vector<std::uint8_t> body) {
  const auto block_length = static_cast<std::uint16_t>(body.size());
  const auto msg_size =
      static_cast<std::uint16_t>(SbeMessageHeader::kEncodedSize + body.size());
  return MsgSpec{msg_size, block_length, template_id, 1, 13, std::move(body)};
}

inline WireBuilder& append(WireBuilder& b, const MsgSpec& m) {
  b.le(m.msg_size)
      .le(m.block_length)
      .le(m.template_id)
      .le(m.schema_id)
      .le(m.version)
      .raw(std::span<const std::uint8_t>{m.body});
  return b;
}

// A datagram: 12-byte binary packet header, then zero or more framed messages.
inline WireBuilder packet(std::uint32_t seq, std::uint64_t sending_time) {
  WireBuilder b;
  b.le(seq).le(sending_time);
  return b;
}

}  // namespace rfeed::test
