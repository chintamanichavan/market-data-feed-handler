#include "rfeed/pcap.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <utility>

namespace rfeed {
namespace {

constexpr std::size_t kGlobalHeaderSize = 24;
constexpr std::size_t kRecordHeaderSize = 16;

constexpr std::uint32_t kMagicMicroLe = 0xA1B2C3D4U;
constexpr std::uint32_t kMagicNanoLe  = 0xA1B23C4DU;
constexpr std::uint32_t kMagicMicroBe = 0xD4C3B2A1U;
constexpr std::uint32_t kMagicNanoBe  = 0x4D3CB2A1U;

constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
constexpr std::uint16_t kEtherTypeVlan = 0x8100;
constexpr std::uint16_t kEtherTypeQinQ = 0x88A8;
constexpr std::uint8_t  kProtoUdp      = 17;

constexpr std::size_t kEthHeaderSize = 14;
constexpr std::size_t kUdpHeaderSize = 8;

[[nodiscard]] std::uint8_t octet(std::span<const std::byte> b, std::size_t off) noexcept {
  return std::to_integer<std::uint8_t>(b[off]);
}

// pcap metadata follows the writer's byte order; IP and UDP headers are always
// big-endian. Both cases go through these.
[[nodiscard]] std::uint16_t rd16(std::span<const std::byte> b, std::size_t off, bool be) noexcept {
  const auto lo = static_cast<std::uint16_t>(octet(b, off));
  const auto hi = static_cast<std::uint16_t>(octet(b, off + 1));
  return be ? static_cast<std::uint16_t>((lo << 8) | hi) : static_cast<std::uint16_t>((hi << 8) | lo);
}

[[nodiscard]] std::uint32_t rd32(std::span<const std::byte> b, std::size_t off, bool be) noexcept {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto shift = be ? (24U - 8U * i) : (8U * i);
    v |= static_cast<std::uint32_t>(octet(b, off + i)) << shift;
  }
  return v;
}

void put16(std::vector<std::byte>& out, std::uint16_t v, bool be) {
  const auto hi = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
  const auto lo = static_cast<std::uint8_t>(v & 0xFFU);
  out.push_back(static_cast<std::byte>(be ? hi : lo));
  out.push_back(static_cast<std::byte>(be ? lo : hi));
}

void put32(std::vector<std::byte>& out, std::uint32_t v, bool be) {
  for (std::size_t i = 0; i < 4; ++i) {
    const auto shift = be ? (24U - 8U * i) : (8U * i);
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((v >> shift) & 0xFFU)));
  }
}

[[nodiscard]] std::uint16_t ones_complement_checksum(std::span<const std::byte> header) noexcept {
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i + 1 < header.size(); i += 2) {
    sum += rd16(header, i, true);
  }
  if (header.size() % 2 != 0) {
    sum += static_cast<std::uint32_t>(octet(header, header.size() - 1)) << 8;
  }
  while ((sum >> 16) != 0) sum = (sum & 0xFFFFU) + (sum >> 16);
  return static_cast<std::uint16_t>(~sum & 0xFFFFU);
}

}  // namespace

std::string ipv4_to_string(std::uint32_t addr) {
  char buf[16] = {};
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (addr >> 24) & 0xFFU, (addr >> 16) & 0xFFU,
                (addr >> 8) & 0xFFU, addr & 0xFFU);
  return std::string{buf};
}

// ─── Reader ──────────────────────────────────────────────────────────

std::expected<PcapReader, PcapError> PcapReader::open(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return std::unexpected{PcapError::Io};

  PcapReader r;
  std::byte  chunk[64 * 1024];
  while (true) {
    const std::size_t n = std::fread(chunk, 1, sizeof(chunk), f);
    if (n == 0) break;
    r.data_.insert(r.data_.end(), chunk, chunk + n);
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) return std::unexpected{PcapError::Io};

  if (r.data_.size() < kGlobalHeaderSize) return std::unexpected{PcapError::TruncatedHeader};

  const std::span<const std::byte> all{r.data_};
  const auto                       magic = rd32(all, 0, false);
  switch (magic) {
    case kMagicMicroLe: r.swapped_ = false; r.nanosecond_ = false; break;
    case kMagicNanoLe:  r.swapped_ = false; r.nanosecond_ = true;  break;
    case kMagicMicroBe: r.swapped_ = true;  r.nanosecond_ = false; break;
    case kMagicNanoBe:  r.swapped_ = true;  r.nanosecond_ = true;  break;
    default: return std::unexpected{PcapError::BadMagic};
  }

  const auto network = rd32(all, 20, r.swapped_);
  if (network != static_cast<std::uint32_t>(LinkType::Ethernet) &&
      network != static_cast<std::uint32_t>(LinkType::Raw)) {
    return std::unexpected{PcapError::UnsupportedLinkType};
  }
  r.link_type_    = static_cast<LinkType>(network);
  r.pos_          = kGlobalHeaderSize;
  r.first_record_ = kGlobalHeaderSize;
  return r;
}

void PcapReader::rewind() noexcept {
  pos_             = first_record_;
  records_read_    = 0;
  records_skipped_ = 0;
}

std::optional<CapturedDatagram> PcapReader::next() noexcept {
  const std::span<const std::byte> all{data_};

  while (pos_ + kRecordHeaderSize <= all.size()) {
    const auto ts_sec   = rd32(all, pos_, swapped_);
    const auto ts_frac  = rd32(all, pos_ + 4, swapped_);
    const auto incl_len = rd32(all, pos_ + 8, swapped_);
    pos_ += kRecordHeaderSize;

    if (static_cast<std::uint64_t>(incl_len) > all.size() - pos_) {
      pos_ = all.size();  // truncated final record; stop cleanly
      return std::nullopt;
    }

    const auto frame = all.subspan(pos_, incl_len);
    pos_ += incl_len;
    ++records_read_;

    const auto ns = static_cast<Timestamp>(ts_sec) * 1'000'000'000 +
                    static_cast<Timestamp>(nanosecond_ ? ts_frac : ts_frac * 1000);

    std::size_t off = 0;
    if (link_type_ == LinkType::Ethernet) {
      if (frame.size() < kEthHeaderSize) { ++records_skipped_; continue; }
      auto ethertype = rd16(frame, 12, true);
      off            = kEthHeaderSize;
      // Up to two stacked VLAN tags, which is what a venue's tap usually adds.
      for (int tag = 0; tag < 2; ++tag) {
        if (ethertype != kEtherTypeVlan && ethertype != kEtherTypeQinQ) break;
        if (frame.size() < off + 4) break;
        ethertype = rd16(frame, off + 2, true);
        off += 4;
      }
      if (ethertype != kEtherTypeIpv4) { ++records_skipped_; continue; }
    }

    if (frame.size() < off + 20) { ++records_skipped_; continue; }

    const auto ver_ihl = octet(frame, off);
    if ((ver_ihl >> 4) != 4) { ++records_skipped_; continue; }
    const auto ihl = static_cast<std::size_t>(ver_ihl & 0x0FU) * 4;
    if (ihl < 20 || frame.size() < off + ihl) { ++records_skipped_; continue; }

    // A fragmented datagram cannot be reassembled from one record, and a
    // half-decoded MDP packet is worse than a dropped one.
    const auto flags_frag = rd16(frame, off + 6, true);
    if ((flags_frag & 0x1FFFU) != 0 || (flags_frag & 0x2000U) != 0) {
      ++records_skipped_;
      continue;
    }

    if (octet(frame, off + 9) != kProtoUdp) { ++records_skipped_; continue; }

    const auto total_length = rd16(frame, off + 2, true);
    const auto src_addr     = rd32(frame, off + 12, true);
    const auto dst_addr     = rd32(frame, off + 16, true);

    const std::size_t udp_off = off + ihl;
    if (frame.size() < udp_off + kUdpHeaderSize) { ++records_skipped_; continue; }

    const auto src_port = rd16(frame, udp_off, true);
    const auto dst_port = rd16(frame, udp_off + 2, true);
    const auto udp_len  = rd16(frame, udp_off + 4, true);

    // Trust whichever length is smaller, and never more than we actually have:
    // captures get snapped, and IP total_length reflects the wire, not the file.
    std::size_t payload_len = (udp_len >= kUdpHeaderSize) ? udp_len - kUdpHeaderSize : 0;
    if (total_length >= ihl + kUdpHeaderSize) {
      payload_len = std::min(payload_len, static_cast<std::size_t>(total_length) - ihl - kUdpHeaderSize);
    }
    payload_len = std::min(payload_len, frame.size() - udp_off - kUdpHeaderSize);

    return CapturedDatagram{
        .payload  = frame.subspan(udp_off + kUdpHeaderSize, payload_len),
        .rx_ts    = ns,
        .src_addr = src_addr,
        .dst_addr = dst_addr,
        .src_port = src_port,
        .dst_port = dst_port,
    };
  }

  return std::nullopt;
}

// ─── Writer ──────────────────────────────────────────────────────────

std::expected<PcapWriter, PcapError> PcapWriter::create(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return std::unexpected{PcapError::Io};

  std::vector<std::byte> header;
  put32(header, kMagicNanoLe, false);  // little-endian, nanosecond resolution
  put16(header, 2, false);             // version major
  put16(header, 4, false);             // version minor
  put32(header, 0, false);             // thiszone
  put32(header, 0, false);             // sigfigs
  put32(header, 262144, false);        // snaplen
  put32(header, static_cast<std::uint32_t>(LinkType::Ethernet), false);

  if (std::fwrite(header.data(), 1, header.size(), f) != header.size()) {
    std::fclose(f);
    return std::unexpected{PcapError::Io};
  }

  PcapWriter w;
  w.file_ = f;
  return w;
}

PcapWriter::PcapWriter(PcapWriter&& other) noexcept : file_{std::exchange(other.file_, nullptr)} {}

PcapWriter& PcapWriter::operator=(PcapWriter&& other) noexcept {
  if (this != &other) {
    if (file_ != nullptr) std::fclose(static_cast<std::FILE*>(file_));
    file_ = std::exchange(other.file_, nullptr);
  }
  return *this;
}

PcapWriter::~PcapWriter() {
  if (file_ != nullptr) std::fclose(static_cast<std::FILE*>(file_));
}

std::expected<void, PcapError> PcapWriter::write_udp(std::span<const std::byte> payload,
                                                     Timestamp rx_ts, std::uint32_t src_addr,
                                                     std::uint16_t src_port, std::uint32_t dst_addr,
                                                     std::uint16_t dst_port) {
  if (file_ == nullptr) return std::unexpected{PcapError::Io};

  const auto udp_len   = static_cast<std::uint16_t>(kUdpHeaderSize + payload.size());
  const auto ip_len    = static_cast<std::uint16_t>(20 + udp_len);

  std::vector<std::byte> frame;
  frame.reserve(kEthHeaderSize + ip_len);

  // Ethernet: a locally-administered unicast source and the standard IPv4
  // multicast destination mapping (01:00:5e | low 23 bits of the group).
  constexpr std::array<std::uint8_t, 3> kMcastPrefix{0x01, 0x00, 0x5E};
  constexpr std::array<std::uint8_t, 6> kSrcMac{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  for (const auto b : kMcastPrefix) frame.push_back(static_cast<std::byte>(b));
  frame.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((dst_addr >> 16) & 0x7FU)));
  frame.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((dst_addr >> 8) & 0xFFU)));
  frame.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(dst_addr & 0xFFU)));
  for (const auto b : kSrcMac) frame.push_back(static_cast<std::byte>(b));
  put16(frame, kEtherTypeIpv4, true);

  const std::size_t ip_off = frame.size();
  frame.push_back(static_cast<std::byte>(0x45U));  // IPv4, IHL 5
  frame.push_back(static_cast<std::byte>(0x00U));  // DSCP/ECN
  put16(frame, ip_len, true);
  put16(frame, 0, true);       // identification
  put16(frame, 0x4000, true);  // don't fragment
  frame.push_back(static_cast<std::byte>(64U));   // TTL
  frame.push_back(static_cast<std::byte>(kProtoUdp));
  put16(frame, 0, true);  // checksum placeholder
  put32(frame, src_addr, true);
  put32(frame, dst_addr, true);

  const auto checksum =
      ones_complement_checksum(std::span<const std::byte>{frame}.subspan(ip_off, 20));
  frame[ip_off + 10] = static_cast<std::byte>(static_cast<std::uint8_t>((checksum >> 8) & 0xFFU));
  frame[ip_off + 11] = static_cast<std::byte>(static_cast<std::uint8_t>(checksum & 0xFFU));

  put16(frame, src_port, true);
  put16(frame, dst_port, true);
  put16(frame, udp_len, true);
  put16(frame, 0, true);  // UDP checksum optional over IPv4
  frame.insert(frame.end(), payload.begin(), payload.end());

  std::vector<std::byte> record;
  put32(record, static_cast<std::uint32_t>(rx_ts / 1'000'000'000), false);
  put32(record, static_cast<std::uint32_t>(rx_ts % 1'000'000'000), false);
  put32(record, static_cast<std::uint32_t>(frame.size()), false);
  put32(record, static_cast<std::uint32_t>(frame.size()), false);

  auto* f = static_cast<std::FILE*>(file_);
  if (std::fwrite(record.data(), 1, record.size(), f) != record.size() ||
      std::fwrite(frame.data(), 1, frame.size(), f) != frame.size()) {
    return std::unexpected{PcapError::Io};
  }
  return {};
}

std::expected<void, PcapError> PcapWriter::close() {
  if (file_ == nullptr) return {};
  const int rc = std::fclose(static_cast<std::FILE*>(file_));
  file_        = nullptr;
  if (rc != 0) return std::unexpected{PcapError::Io};
  return {};
}

}  // namespace rfeed
