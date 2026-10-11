#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "rfeed/byte_reader.hpp"
#include "rfeed/sbe_header.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

// Binary packet header — 12 bytes, little-endian, one per datagram.
//
//   uint32 MsgSeqNum     per channel, per feed; the A/B arbitration key
//   uint64 SendingTime   ns since epoch, venue clock
//
// ASSUMPTION (flagged, verify against the new venue's spec before trusting):
// this is CME's layout. A venue that is "functionally identical" may still
// differ here, and a wrong header size desynchronises every packet silently.
struct PacketHeader {
  SeqNum    seq{};
  Timestamp sending_time{};

  static constexpr std::size_t kEncodedSize = 12;

  [[nodiscard]] static std::expected<PacketHeader, DecodeError> decode(ByteReader& in) noexcept;

  auto operator<=>(const PacketHeader&) const = default;
};

// One framed message. `body` is the root block plus any repeating groups — the
// 8-byte SBE header has already been consumed. Non-owning; valid only as long as
// the underlying datagram is.
struct MessageView {
  SbeMessageHeader           hdr{};
  std::span<const std::byte> body{};
};

// Forward iterator over the message region of a single packet.
//
// Framing is where malformed input bites: MsgSize can lie, overrun the datagram,
// or be zero. Every advance is validated against the remaining span, and any
// inconsistency latches the framer permanently — a bad packet can never spin the
// caller's loop or read past the buffer.
//
// ASSUMPTION: MsgSize counts the SBE header plus the body, and does not include
// its own 2 bytes. This is CME's convention. If the new venue excludes the SBE
// header from MsgSize, every message after the first in a packet misframes.
class MessageFramer {
 public:
  MessageFramer() noexcept = default;

  explicit constexpr MessageFramer(std::span<const std::byte> messages) noexcept
      : reader_{messages} {}

  // Returns the next message; DecodeError::EndOfPacket on clean exhaustion; any
  // other DecodeError on a malformed packet. After a fault every subsequent call
  // returns that same error and the cursor never moves again.
  [[nodiscard]] std::expected<MessageView, DecodeError> next() noexcept;

  [[nodiscard]] constexpr bool failed() const noexcept { return failed_; }
  [[nodiscard]] constexpr std::size_t bytes_consumed() const noexcept { return reader_.offset(); }

 private:
  ByteReader  reader_{};
  bool        failed_{false};
  DecodeError error_{DecodeError::EndOfPacket};
};

// A decoded datagram: header plus the undecoded message region.
struct PacketView {
  PacketHeader               header{};
  std::span<const std::byte> messages{};

  [[nodiscard]] constexpr MessageFramer framer() const noexcept { return MessageFramer{messages}; }

  // A header-only datagram is legal and normal — that is a heartbeat.
  [[nodiscard]] constexpr bool is_heartbeat() const noexcept { return messages.empty(); }

  [[nodiscard]] static std::expected<PacketView, DecodeError> decode(
      std::span<const std::byte> datagram) noexcept;
};

}  // namespace rfeed
