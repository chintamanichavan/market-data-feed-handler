#pragma once

// Live multicast transport.
//
// Deliberately the LAST thing built. Everything upstream of it — framing,
// decode, arbitration, book building, recovery — was developed and tested
// against captures through PcapTransport, and this file changes none of it: it
// satisfies the same Transport concept, so ChannelHandler does not know which
// one it is holding.
//
// Linux only. The three things that make a market-data receiver different from
// a textbook UDP reader are all Linux-specific:
//   - recvmmsg    : one syscall per batch instead of one per datagram
//   - SO_RXQ_OVFL : the kernel tells you how many datagrams it dropped, so
//                   receiver-side loss is distinguishable from network loss
//   - SO_TIMESTAMPING : NIC hardware timestamps, so rx_ts measures when the
//                   packet arrived rather than when we got round to reading it
//
// Without the second one, an overloaded consumer looks exactly like a lossy
// network, and you spend a week blaming the venue.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

#include "rfeed/channel_config.hpp"
#include "rfeed/transport.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

enum class TransportError : std::uint8_t {
  Unsupported,    // not built for this platform
  NoConnections,  // the channel config describes no multicast endpoints
  SocketCreate,
  SetSockOpt,
  Bind,
  JoinGroup,
};

[[nodiscard]] constexpr std::string_view to_string(TransportError e) noexcept {
  switch (e) {
    case TransportError::Unsupported:   return "Unsupported";
    case TransportError::NoConnections: return "NoConnections";
    case TransportError::SocketCreate:  return "SocketCreate";
    case TransportError::SetSockOpt:    return "SetSockOpt";
    case TransportError::Bind:          return "Bind";
    case TransportError::JoinGroup:     return "JoinGroup";
  }
  return "Unknown";
}

// Where rx_ts actually came from. Worth surfacing: a latency number measured
// against a software timestamp is measuring your own scheduler as much as the
// network, and reporting it as though it were hardware is how a feed handler
// acquires an undeserved reputation.
enum class TimestampSource : std::uint8_t { Hardware, KernelSoftware, Userspace };

struct UdpConfig {
  // Local interface address for the IGMP join, host byte order. 0 lets the
  // kernel route it — fine on a single-homed box, wrong on any trading host,
  // which will have a separate market-data NIC.
  std::uint32_t interface{0};

  // Receive buffer. The default here is far above the system default on
  // purpose: a burst at the open will overrun a 200KB buffer, and the resulting
  // loss looks identical to a network gap. Requires net.core.rmem_max to be
  // raised or the kernel silently clamps it — which is why the value actually
  // granted is reported back through rcvbuf_granted().
  int rcvbuf_bytes{16 * 1024 * 1024};

  // Datagrams per recvmmsg call.
  std::size_t batch{32};

  bool hardware_timestamps{true};

  // Needed when several processes on the box consume the same channel.
  bool reuse_port{true};
};

struct UdpStats {
  std::uint64_t datagrams{0};
  std::uint64_t batches{0};
  std::uint64_t truncated{0};
  // Cumulative kernel drop count from SO_RXQ_OVFL: datagrams the NIC delivered
  // and the socket buffer had no room for. Loss here is OURS, not the venue's.
  std::uint64_t kernel_drops{0};
};

class UdpTransport {
 public:
  [[nodiscard]] static std::expected<UdpTransport, TransportError> open(ChannelConfig config,
                                                                        UdpConfig options = {});

  UdpTransport(UdpTransport&&) noexcept;
  UdpTransport& operator=(UdpTransport&&) noexcept;
  UdpTransport(const UdpTransport&)            = delete;
  UdpTransport& operator=(const UdpTransport&) = delete;
  ~UdpTransport();

  [[nodiscard]] std::optional<RawPacket> poll() noexcept;

  // A live socket is never exhausted. Returning true here would make
  // ChannelHandler::run() exit the moment the market went quiet.
  [[nodiscard]] bool exhausted() const noexcept { return false; }

  // Blocks until any socket is readable or the timeout expires. Returns false
  // on timeout. Callers that block must still drive ChannelHandler::on_clock(),
  // or a feed that goes silent is never noticed.
  [[nodiscard]] bool wait(int timeout_ms) noexcept;

  [[nodiscard]] const UdpStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const ChannelConfig& config() const noexcept { return config_; }
  [[nodiscard]] TimestampSource timestamp_source() const noexcept { return ts_source_; }
  [[nodiscard]] int rcvbuf_granted() const noexcept { return rcvbuf_granted_; }
  [[nodiscard]] std::size_t socket_count() const noexcept { return sockets_.size(); }

 private:
  struct Socket {
    int         fd{-1};
    LineId      line{LineId::A};
    ChannelKind kind{ChannelKind::Incremental};
  };

  UdpTransport() = default;

  // Fills the batch from one socket. Returns the number of datagrams read.
  std::size_t receive_batch(std::size_t socket_index) noexcept;
  void        close_all() noexcept;

  struct Received {
    std::size_t offset{0};
    std::size_t len{0};
    Timestamp   rx_ts{0};
    LineId      line{LineId::A};
    ChannelKind kind{ChannelKind::Incremental};
  };

  ChannelConfig          config_{};
  UdpConfig              options_{};
  std::vector<Socket>    sockets_{};
  std::vector<std::byte> storage_{};   // options_.batch * kMaxDatagramBytes
  std::vector<Received>  batch_{};
  std::size_t            batch_pos_{0};
  std::size_t            next_socket_{0};
  UdpStats               stats_{};
  TimestampSource        ts_source_{TimestampSource::Userspace};
  int                    rcvbuf_granted_{0};
  std::uint32_t          last_overflow_{0};
  bool                   overflow_primed_{false};
};

static_assert(Transport<UdpTransport>);

}  // namespace rfeed
