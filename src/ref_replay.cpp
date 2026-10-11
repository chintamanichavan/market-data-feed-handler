#include "ref_replay.hpp"

#include <algorithm>
#include <cstring>

#include "rfeed/byte_reader.hpp"

namespace rfeed::venue_ref {
namespace {

constexpr std::array<std::byte, 4> kRequestMagic{std::byte{'R'}, std::byte{'F'}, std::byte{'R'},
                                                 std::byte{'Q'}};
constexpr std::array<std::byte, 4> kResponseMagic{std::byte{'R'}, std::byte{'F'}, std::byte{'R'},
                                                  std::byte{'S'}};

void put_u16(std::vector<std::byte>& out, std::uint16_t v) {
  out.push_back(static_cast<std::byte>(v & 0xFF));
  out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((v >> shift) & 0xFF));
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                    static_cast<std::uint16_t>(
                                        static_cast<std::uint16_t>(p[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* p) noexcept {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) {
    v = (v << 8) | static_cast<std::uint32_t>(p[static_cast<std::size_t>(i)]);
  }
  return v;
}

}  // namespace

void ReplayParser::reset() noexcept {
  pending_.clear();
  packets_.clear();
  expected_    = 0;
  status_      = ReplayStatus::Unknown;
  header_seen_ = false;
}

bool ReplayParser::consume(std::span<const std::byte> in) {
  pending_.insert(pending_.end(), in.begin(), in.end());
  std::size_t offset = 0;

  if (!header_seen_) {
    if (pending_.size() < kReplayResponseBytes) return true;  // wait for more
    if (!std::equal(kResponseMagic.begin(), kResponseMagic.end(), pending_.begin())) return false;

    const auto raw = get_u16(pending_.data() + 4);
    switch (raw) {
      case 0:  status_ = ReplayStatus::Ok; break;
      case 1:  status_ = ReplayStatus::Refused; break;
      case 2:  status_ = ReplayStatus::RateLimited; break;
      case 3:  status_ = ReplayStatus::NotAvailable; break;
      default: return false;  // an unknown status is not something to guess at
    }
    expected_    = get_u32(pending_.data() + 8);
    header_seen_ = true;
    offset       = kReplayResponseBytes;

    // A refusal carries no records. Treating a non-Ok status as though records
    // were coming leaves the client waiting for a body that will never arrive.
    if (status_ != ReplayStatus::Ok) expected_ = 0;
  }

  while (packets_.size() < expected_) {
    if (pending_.size() - offset < 2) break;
    const auto len = get_u16(pending_.data() + offset);
    if (len == 0 || len > kMaxDatagramBytes) return false;
    if (pending_.size() - offset < 2U + len) break;

    const auto* body = pending_.data() + offset + 2;
    packets_.emplace_back(body, body + len);
    offset += 2U + len;
  }

  pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
  return true;
}

void ReplayParser::encode_request(std::vector<std::byte>& out, ChannelId channel,
                                  const GapRange& gap) {
  out.insert(out.end(), kRequestMagic.begin(), kRequestMagic.end());
  put_u16(out, channel);
  put_u32(out, gap.begin);
  put_u32(out, gap.count());
  put_u16(out, 0);  // pad to 16
}

void ReplayParser::encode_response_header(std::vector<std::byte>& out, ReplayStatus status,
                                          std::uint32_t packets) {
  out.insert(out.end(), kResponseMagic.begin(), kResponseMagic.end());
  put_u16(out, static_cast<std::uint16_t>(status));
  put_u16(out, 0);
  put_u32(out, packets);
}

void ReplayParser::encode_record(std::vector<std::byte>& out, std::span<const std::byte> datagram) {
  put_u16(out, static_cast<std::uint16_t>(datagram.size()));
  out.insert(out.end(), datagram.begin(), datagram.end());
}

std::optional<ReplayParser::Request> ReplayParser::decode_request(std::span<const std::byte> in) {
  if (in.size() < kReplayRequestBytes) return std::nullopt;
  if (!std::equal(kRequestMagic.begin(), kRequestMagic.end(), in.begin())) return std::nullopt;
  return Request{
      .channel = get_u16(in.data() + 4),
      .begin   = get_u32(in.data() + 6),
      .count   = get_u32(in.data() + 10),
  };
}

}  // namespace rfeed::venue_ref
