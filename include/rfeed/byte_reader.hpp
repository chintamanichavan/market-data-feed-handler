#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

#include "rfeed/types.hpp"

namespace rfeed {

template <class T>
concept WireInt = std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;

// Bounds-checked, forward-only cursor over a byte span.
//
// This is the only type in the handler that touches unvalidated wire bytes, so
// it is the only one that has to be paranoid. Every read is checked against the
// remaining span and fails with a DecodeError instead of reading out of bounds.
// On failure the cursor does not advance, so a caller may inspect and bail
// without the reader having been left in a torn state.
//
// Reads are byte-assembled rather than memcpy'd, which keeps them constexpr and
// keeps them correct on a big-endian host. Both GCC and Clang fold the loop into
// a single load on little-endian targets.
//
// Holds a non-owning view. The referenced bytes must outlive the reader — on the
// hot path they point straight into the transport's receive buffer, which is
// recycled on the next poll().
class ByteReader {
 public:
  ByteReader() noexcept = default;

  explicit constexpr ByteReader(std::span<const std::byte> bytes) noexcept : bytes_{bytes} {}

  template <WireInt T>
  [[nodiscard]] constexpr std::expected<T, DecodeError> read() noexcept {
    if (remaining() < sizeof(T)) return std::unexpected{DecodeError::Truncated};
    using U = std::make_unsigned_t<T>;
    U value{0};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      const auto octet = static_cast<U>(std::to_integer<std::uint8_t>(bytes_[pos_ + i]));
      value = static_cast<U>(value | static_cast<U>(octet << (8U * i)));
    }
    pos_ += sizeof(T);
    return static_cast<T>(value);
  }

  [[nodiscard]] constexpr std::expected<std::span<const std::byte>, DecodeError> read_bytes(
      std::size_t n) noexcept {
    if (remaining() < n) return std::unexpected{DecodeError::Truncated};
    const auto out = bytes_.subspan(pos_, n);
    pos_ += n;
    return out;
  }

  [[nodiscard]] constexpr std::expected<void, DecodeError> skip(std::size_t n) noexcept {
    if (remaining() < n) return std::unexpected{DecodeError::Truncated};
    pos_ += n;
    return {};
  }

  [[nodiscard]] constexpr std::size_t remaining() const noexcept { return bytes_.size() - pos_; }
  [[nodiscard]] constexpr std::size_t offset() const noexcept { return pos_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return remaining() == 0; }

  // Everything from the cursor to the end, without advancing.
  [[nodiscard]] constexpr std::span<const std::byte> rest() const noexcept {
    return bytes_.subspan(pos_);
  }

 private:
  std::span<const std::byte> bytes_{};
  std::size_t                pos_{0};
};

}  // namespace rfeed
