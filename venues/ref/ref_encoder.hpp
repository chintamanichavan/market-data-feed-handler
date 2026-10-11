#pragma once

// Encoder for the reference venue.
//
// This is the mitigation for the largest schedule risk in the project: without
// a capture from the venue, every phase after framing is written blind. A
// first-class synthetic packet generator makes Phases 4–7 testable on day one,
// and stays useful afterwards as the source of deterministic fixtures.
//
// It is also a real test oracle. Encode → decode → compare exercises both
// directions against one schema, so a layout misreading has to be made twice,
// identically, to go unnoticed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"

namespace rfeed::venue_ref {

class Encoder {
 public:
  // Starts a datagram: 12-byte binary packet header, no messages yet. A packet
  // left with no messages is a valid heartbeat.
  Encoder(SeqNum seq, Timestamp sending_time);

  Encoder& incremental(const IncrementalHeader& header, std::span<const MdEntry> entries);
  Encoder& snapshot(const SnapshotHeader& header, std::span<const SnapshotEntry> entries);
  Encoder& instrument_definition(const InstrumentDefinition& def);
  Encoder& security_status(const SecurityStatusMsg& msg);
  Encoder& channel_reset(const ChannelResetMsg& msg);
  Encoder& admin_heartbeat();

  [[nodiscard]] std::span<const std::byte> view() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] std::uint16_t message_count() const noexcept { return messages_; }

 private:
  std::vector<std::byte> bytes_{};
  std::uint16_t          messages_{0};
};

// Convenience for the common single-message-per-packet case.
[[nodiscard]] std::vector<std::byte> encode_incremental_packet(SeqNum seq, Timestamp sending_time,
                                                               const IncrementalHeader& header,
                                                               std::span<const MdEntry> entries);

}  // namespace rfeed::venue_ref
