#pragma once

// Wire format for the reference venue's TCP replay service.
//
// Replay framing is per-venue and has nothing in common across venues — CME
// wraps it in a FIX logon, others use a bespoke binary request, others again
// make you call an operator. So it lives here, behind a concept, and the core
// ReplayClient below knows only "encode a request" and "feed me bytes".
//
// Reference framing, all little-endian:
//
//   Request   'R''F''R''Q' | u16 channel | u32 begin | u32 count | u16 pad   (16B)
//   Response  'R''F''R''S' | u16 status  | u16 pad   | u32 packets          (12B)
//             then `packets` records of: u16 length | length bytes
//
// A record's payload is exactly the UDP datagram the incremental feed would
// have carried, so recovered packets rejoin the pipeline at the same point as
// live ones and share every line of decode and book-building code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "rfeed/line_arbiter.hpp"
#include "rfeed/transport.hpp"
#include "rfeed/types.hpp"

namespace rfeed::venue_ref {

inline constexpr std::size_t kReplayRequestBytes  = 16;
inline constexpr std::size_t kReplayResponseBytes = 12;

enum class ReplayStatus : std::uint16_t {
  Ok          = 0,
  Refused     = 1,
  RateLimited = 2,
  NotAvailable = 3,
  Unknown     = 0xFFFF,
};

[[nodiscard]] constexpr std::string_view to_string(ReplayStatus s) noexcept {
  switch (s) {
    case ReplayStatus::Ok:           return "Ok";
    case ReplayStatus::Refused:      return "Refused";
    case ReplayStatus::RateLimited:  return "RateLimited";
    case ReplayStatus::NotAvailable: return "NotAvailable";
    case ReplayStatus::Unknown:      return "Unknown";
  }
  return "Unknown";
}

// Incremental response parser. TCP delivers a byte stream with no respect for
// message boundaries, so every field has to survive being split across reads —
// which is exactly the bug that only ever shows up in production, under load,
// when the response finally exceeds one segment.
class ReplayParser {
 public:
  // The status the core treats as "records follow". Every other value is a
  // refusal of some kind, and the core does not need to know which.
  static constexpr ReplayStatus kOkStatus = ReplayStatus::Ok;

  void reset() noexcept;

  // Feeds received bytes. Returns false on a protocol violation, after which
  // the session must be torn down rather than resynchronised: there is no
  // framing marker to resynchronise to.
  [[nodiscard]] bool consume(std::span<const std::byte> in);

  [[nodiscard]] bool header_seen() const noexcept { return header_seen_; }
  [[nodiscard]] ReplayStatus status() const noexcept { return status_; }
  [[nodiscard]] std::uint32_t expected_packets() const noexcept { return expected_; }
  [[nodiscard]] bool complete() const noexcept {
    return header_seen_ && packets_.size() >= expected_;
  }

  // Recovered datagrams, in sequence order. Heap-allocated on purpose: this is
  // the cold path by construction — if replay is hot, the problem is upstream.
  [[nodiscard]] const std::vector<std::vector<std::byte>>& packets() const noexcept {
    return packets_;
  }
  [[nodiscard]] std::vector<std::vector<std::byte>> take() noexcept { return std::move(packets_); }

  // Builds a request. Static because it needs no session state.
  static void encode_request(std::vector<std::byte>& out, ChannelId channel, const GapRange& gap);

  // Server side, used by the test harness and by tools/rfeed_replayd.
  static void encode_response_header(std::vector<std::byte>& out, ReplayStatus status,
                                     std::uint32_t packets);
  static void encode_record(std::vector<std::byte>& out, std::span<const std::byte> datagram);

  struct Request {
    ChannelId     channel{};
    SeqNum        begin{};
    std::uint32_t count{};
  };
  [[nodiscard]] static std::optional<Request> decode_request(std::span<const std::byte> in);

 private:
  std::vector<std::byte>              pending_{};  // bytes not yet forming a whole field
  std::vector<std::vector<std::byte>> packets_{};
  std::uint32_t                       expected_{0};
  ReplayStatus                        status_{ReplayStatus::Unknown};
  bool                                header_seen_{false};
};

}  // namespace rfeed::venue_ref
