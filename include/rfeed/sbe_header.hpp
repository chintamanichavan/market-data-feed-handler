#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "rfeed/byte_reader.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

// SBE message header — 8 bytes, little-endian.
//
// Hand-written on purpose. Message *bodies* are generated from the venue's SBE
// schema XML, but the header must be readable without entering the generated
// namespace: dispatch and version checking happen before we know which venue's
// codec applies, and two venues produce two mutually incompatible namespaces.
//
//   uint16 BlockLength   size of the fixed root block (groups follow it)
//   uint16 TemplateId    which message; NEVER hard-code these, resolve via the
//                        generated template registry — venues renumber
//   uint16 SchemaId
//   uint16 Version       acting version; used for version-tolerant field access
struct SbeMessageHeader {
  std::uint16_t block_length{};
  std::uint16_t template_id{};
  std::uint16_t schema_id{};
  std::uint16_t version{};

  static constexpr std::size_t kEncodedSize = 8;

  [[nodiscard]] static std::expected<SbeMessageHeader, DecodeError> decode(ByteReader& in) noexcept;

  auto operator<=>(const SbeMessageHeader&) const = default;
};

}  // namespace rfeed
