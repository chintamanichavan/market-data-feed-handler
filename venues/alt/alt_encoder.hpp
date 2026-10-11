#pragma once

// Encoder for the second reference venue. Mirrors alt_codec.cpp field for field,
// so encode → decode → compare is a genuine round trip: a layout misreading has
// to be made twice, identically and in opposite directions, to go unnoticed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"

namespace rfeed::venue_alt {

class Encoder {
 public:
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

}  // namespace rfeed::venue_alt
