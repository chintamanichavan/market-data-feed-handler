#pragma once

#include <cstdint>

namespace rfeed {

// Plain counters, in the style of rtrade::Telemetry. Deliberately not atomic:
// each channel is owned by one thread, and the reader tolerates a torn read.
//
// A feed handler that cannot distinguish "the venue gapped" from "my socket
// buffer overflowed" is undebuggable, so these are load-bearing, not decoration.
struct DecodeStats {
  std::uint64_t packets_decoded{0};
  std::uint64_t packets_rejected{0};   // header malformed / datagram too short
  std::uint64_t heartbeats{0};         // header-only datagrams
  std::uint64_t messages_decoded{0};
  std::uint64_t messages_rejected{0};  // framing fault; rest of packet abandoned
  std::uint64_t bytes_decoded{0};
};

}  // namespace rfeed
