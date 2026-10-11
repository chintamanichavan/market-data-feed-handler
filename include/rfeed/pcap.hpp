#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rfeed/types.hpp"

namespace rfeed {

// Classic libpcap savefile support, hand-rolled.
//
// No libpcap dependency: the format is ~24 bytes of global header and ~16 per
// record, and owning the parser means the replay path has no external runtime
// and no version skew. Handles both byte orders and both the microsecond
// (0xa1b2c3d4) and nanosecond (0xa1b23c4d) magics, because captures arrive from
// whatever tcpdump the venue happened to run.

enum class LinkType : std::uint32_t {
  Ethernet = 1,
  Raw      = 101,  // raw IPv4, no link header
};

enum class PcapError : std::uint8_t {
  Io,
  BadMagic,
  UnsupportedLinkType,
  TruncatedHeader,
  TruncatedRecord,
};

[[nodiscard]] constexpr std::string_view to_string(PcapError e) noexcept {
  switch (e) {
    case PcapError::Io:                  return "Io";
    case PcapError::BadMagic:            return "BadMagic";
    case PcapError::UnsupportedLinkType: return "UnsupportedLinkType";
    case PcapError::TruncatedHeader:     return "TruncatedHeader";
    case PcapError::TruncatedRecord:     return "TruncatedRecord";
  }
  return "Unknown";
}

// A UDP datagram lifted out of a capture. `payload` points into the reader's
// buffer and stays valid for the reader's lifetime.
struct CapturedDatagram {
  std::span<const std::byte> payload{};
  Timestamp                  rx_ts{};  // ns since epoch, from the capture
  std::uint32_t              src_addr{};
  std::uint32_t              dst_addr{};  // the multicast group
  std::uint16_t              src_port{};
  std::uint16_t              dst_port{};
};

// Reads a whole capture into memory once, then hands out non-owning views.
// Captures are replayed many times during development; paying the read cost per
// run rather than per packet keeps the replay loop honest.
class PcapReader {
 public:
  [[nodiscard]] static std::expected<PcapReader, PcapError> open(const std::string& path);

  // Next UDP datagram, or nullopt at end of capture. Non-UDP, non-IPv4, and
  // fragmented packets are skipped silently but counted.
  [[nodiscard]] std::optional<CapturedDatagram> next() noexcept;

  [[nodiscard]] std::uint64_t records_read() const noexcept { return records_read_; }
  [[nodiscard]] std::uint64_t records_skipped() const noexcept { return records_skipped_; }
  [[nodiscard]] LinkType link_type() const noexcept { return link_type_; }

  void rewind() noexcept;

 private:
  std::vector<std::byte> data_{};
  std::size_t            pos_{0};
  std::size_t            first_record_{0};
  LinkType               link_type_{LinkType::Ethernet};
  bool                   swapped_{false};
  bool                   nanosecond_{false};
  std::uint64_t          records_read_{0};
  std::uint64_t          records_skipped_{0};
};

// Writes captures the reader can consume. Used by tools/rfeed_gen to build
// synthetic feeds, which is the mitigation for not having a venue capture.
class PcapWriter {
 public:
  [[nodiscard]] static std::expected<PcapWriter, PcapError> create(const std::string& path);

  PcapWriter(const PcapWriter&)            = delete;
  PcapWriter& operator=(const PcapWriter&) = delete;
  PcapWriter(PcapWriter&&) noexcept;
  PcapWriter& operator=(PcapWriter&&) noexcept;
  ~PcapWriter();

  // Synthesises Ethernet + IPv4 + UDP framing around `payload`.
  [[nodiscard]] std::expected<void, PcapError> write_udp(std::span<const std::byte> payload,
                                                         Timestamp     rx_ts,
                                                         std::uint32_t src_addr,
                                                         std::uint16_t src_port,
                                                         std::uint32_t dst_addr,
                                                         std::uint16_t dst_port);

  [[nodiscard]] std::expected<void, PcapError> close();

 private:
  PcapWriter() = default;
  void* file_{nullptr};  // FILE*, kept opaque so <cstdio> stays out of the header
};

// 192.0.2.0/24 is TEST-NET-1; 224.0.x.y is multicast. Helper for tools.
[[nodiscard]] constexpr std::uint32_t ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                                           std::uint8_t d) noexcept {
  return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
         (static_cast<std::uint32_t>(c) << 8) | static_cast<std::uint32_t>(d);
}

[[nodiscard]] std::string ipv4_to_string(std::uint32_t addr);

}  // namespace rfeed
