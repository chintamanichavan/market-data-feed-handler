#include "rfeed/transport_udp.hpp"

#ifndef __linux__

namespace rfeed {
std::expected<UdpTransport, TransportError> UdpTransport::open(ChannelConfig, UdpConfig) {
  return std::unexpected{TransportError::Unsupported};
}
UdpTransport::UdpTransport(UdpTransport&&) noexcept = default;
UdpTransport& UdpTransport::operator=(UdpTransport&&) noexcept = default;
UdpTransport::~UdpTransport() = default;
std::optional<RawPacket> UdpTransport::poll() noexcept { return std::nullopt; }
bool UdpTransport::wait(int) noexcept { return false; }
std::size_t UdpTransport::receive_batch(std::size_t) noexcept { return 0; }
void UdpTransport::close_all() noexcept {}
}  // namespace rfeed

#else

#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <utility>

namespace rfeed {
namespace {

// Room for a scm_timestamping (3 timespecs) plus an SO_RXQ_OVFL word, with
// headroom. Undersizing this sets MSG_CTRUNC and silently loses the timestamp.
constexpr std::size_t kControlLen = 256;

// The payload of an SO_TIMESTAMPING control message. Declared here rather than
// pulled from <linux/errqueue.h>, which moved this definition between kernel
// header versions and drags in errqueue types that clash with <netinet/in.h> on
// some distributions. The layout is kernel ABI and cannot change.
struct ScmTimestamping {
  timespec ts[3];  // [0] software, [1] deprecated, [2] raw hardware
};

[[nodiscard]] Timestamp now_realtime_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

[[nodiscard]] Timestamp to_ns(const timespec& ts) noexcept {
  return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Tries hardware timestamping, then kernel software, then nothing. Reports which
// one actually took, because the caller's latency numbers mean different things
// in each case.
[[nodiscard]] TimestampSource enable_timestamps(int fd, bool want_hardware) noexcept {
  if (want_hardware) {
    const int flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                      SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0) {
      return TimestampSource::Hardware;
    }
  }
  const int on = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) == 0) {
    return TimestampSource::KernelSoftware;
  }
  return TimestampSource::Userspace;
}

struct CmsgResult {
  std::optional<Timestamp>     rx_ts{};
  std::optional<std::uint32_t> overflow{};
};

CmsgResult parse_control(msghdr& hdr) noexcept {
  CmsgResult out;
  for (cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr); cmsg != nullptr; cmsg = CMSG_NXTHDR(&hdr, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET) continue;

    if (cmsg->cmsg_type == SO_TIMESTAMPING) {
      ScmTimestamping stamps{};
      std::memcpy(&stamps, CMSG_DATA(cmsg), sizeof(stamps));
      // ts[2] is the raw hardware stamp, ts[0] the software one. Prefer
      // hardware; a NIC without PTP support leaves ts[2] zeroed.
      if (stamps.ts[2].tv_sec != 0 || stamps.ts[2].tv_nsec != 0) {
        out.rx_ts = to_ns(stamps.ts[2]);
      } else if (stamps.ts[0].tv_sec != 0 || stamps.ts[0].tv_nsec != 0) {
        out.rx_ts = to_ns(stamps.ts[0]);
      }
    } else if (cmsg->cmsg_type == SO_TIMESTAMPNS) {
      timespec ts{};
      std::memcpy(&ts, CMSG_DATA(cmsg), sizeof(ts));
      out.rx_ts = to_ns(ts);
    } else if (cmsg->cmsg_type == SO_RXQ_OVFL) {
      std::uint32_t drops = 0;
      std::memcpy(&drops, CMSG_DATA(cmsg), sizeof(drops));
      out.overflow = drops;
    }
  }
  return out;
}

[[nodiscard]] std::expected<int, TransportError> make_socket(const Endpoint& endpoint,
                                                             const UdpConfig& options,
                                                             TimestampSource& ts_source,
                                                             int&             rcvbuf_granted) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (fd < 0) return std::unexpected{TransportError::SocketCreate};

  auto fail = [fd](TransportError e) {
    ::close(fd);
    return std::unexpected<TransportError>{e};
  };

  const int on = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
    return fail(TransportError::SetSockOpt);
  }
  if (options.reuse_port &&
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0) {
    return fail(TransportError::SetSockOpt);
  }

  // Ask the kernel to report its own drops. Without this, receiver-side loss is
  // indistinguishable from network loss and gets blamed on the venue.
  if (::setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on)) != 0) {
    return fail(TransportError::SetSockOpt);
  }

  if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &options.rcvbuf_bytes,
                   sizeof(options.rcvbuf_bytes)) != 0) {
    return fail(TransportError::SetSockOpt);
  }
  // The kernel clamps to net.core.rmem_max without telling anyone, so read back
  // what was actually granted rather than assuming the request was honoured.
  {
    int       granted = 0;
    socklen_t len     = sizeof(granted);
    if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &granted, &len) == 0) {
      rcvbuf_granted = granted / 2;  // Linux reports double the usable size
    }
  }

  ts_source = enable_timestamps(fd, options.hardware_timestamps);

  // Bind to the group address, not INADDR_ANY: binding to the wildcard delivers
  // every datagram destined for this port on any group the host has joined, so
  // two channels sharing a port number would silently cross-feed each other.
  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = ::htons(endpoint.port);
  addr.sin_addr.s_addr = ::htonl(endpoint.group);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return fail(TransportError::Bind);
  }

  if (endpoint.source != 0) {
    ip_mreq_source mreq{};
    mreq.imr_multiaddr.s_addr  = ::htonl(endpoint.group);
    mreq.imr_interface.s_addr  = ::htonl(options.interface);
    mreq.imr_sourceaddr.s_addr = ::htonl(endpoint.source);
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
      return fail(TransportError::JoinGroup);
    }
  } else {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::htonl(endpoint.group);
    mreq.imr_interface.s_addr = ::htonl(options.interface);
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
      return fail(TransportError::JoinGroup);
    }
  }

  return fd;
}

}  // namespace

std::expected<UdpTransport, TransportError> UdpTransport::open(ChannelConfig config,
                                                               UdpConfig     options) {
  if (options.batch == 0) options.batch = 1;

  UdpTransport transport;
  transport.options_ = options;
  transport.config_  = std::move(config);

  std::size_t multicast_connections = 0;
  for (const auto& conn : transport.config_.connections) {
    // The replay service is TCP; it is ReplayClient's business, not ours.
    if (conn.kind == ChannelKind::HistoricalReplay) continue;
    if (!conn.endpoint.valid()) continue;
    ++multicast_connections;

    auto fd = make_socket(conn.endpoint, options, transport.ts_source_,
                          transport.rcvbuf_granted_);
    if (!fd) {
      transport.close_all();
      return std::unexpected{fd.error()};
    }
    transport.sockets_.push_back(Socket{*fd, conn.line, conn.kind});
  }

  if (multicast_connections == 0) return std::unexpected{TransportError::NoConnections};

  transport.storage_.resize(options.batch * kMaxDatagramBytes);
  transport.batch_.reserve(options.batch);
  return transport;
}

UdpTransport::UdpTransport(UdpTransport&& other) noexcept
    : config_{std::move(other.config_)},
      options_{other.options_},
      sockets_{std::move(other.sockets_)},
      storage_{std::move(other.storage_)},
      batch_{std::move(other.batch_)},
      batch_pos_{other.batch_pos_},
      next_socket_{other.next_socket_},
      stats_{other.stats_},
      ts_source_{other.ts_source_},
      rcvbuf_granted_{other.rcvbuf_granted_},
      last_overflow_{other.last_overflow_},
      overflow_primed_{other.overflow_primed_} {
  other.sockets_.clear();
}

UdpTransport& UdpTransport::operator=(UdpTransport&& other) noexcept {
  if (this == &other) return *this;
  close_all();
  config_          = std::move(other.config_);
  options_         = other.options_;
  sockets_         = std::move(other.sockets_);
  storage_         = std::move(other.storage_);
  batch_           = std::move(other.batch_);
  batch_pos_       = other.batch_pos_;
  next_socket_     = other.next_socket_;
  stats_           = other.stats_;
  ts_source_       = other.ts_source_;
  rcvbuf_granted_  = other.rcvbuf_granted_;
  last_overflow_   = other.last_overflow_;
  overflow_primed_ = other.overflow_primed_;
  other.sockets_.clear();
  return *this;
}

UdpTransport::~UdpTransport() { close_all(); }

void UdpTransport::close_all() noexcept {
  for (auto& s : sockets_) {
    if (s.fd >= 0) ::close(s.fd);
    s.fd = -1;
  }
  sockets_.clear();
}

std::size_t UdpTransport::receive_batch(std::size_t socket_index) noexcept {
  auto& sock = sockets_[socket_index];

  const std::size_t          count = options_.batch;
  std::vector<mmsghdr>       msgs(count);
  std::vector<iovec>         iovs(count);
  std::vector<std::array<char, kControlLen>> control(count);

  for (std::size_t i = 0; i < count; ++i) {
    iovs[i].iov_base           = storage_.data() + i * kMaxDatagramBytes;
    iovs[i].iov_len            = kMaxDatagramBytes;
    msgs[i].msg_hdr.msg_iov    = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_control    = control[i].data();
    msgs[i].msg_hdr.msg_controllen = control[i].size();
  }

  const int received = ::recvmmsg(sock.fd, msgs.data(), static_cast<unsigned>(count),
                                  MSG_DONTWAIT, nullptr);
  if (received <= 0) return 0;

  ++stats_.batches;
  batch_.clear();
  batch_pos_ = 0;

  for (int i = 0; i < received; ++i) {
    const auto parsed = parse_control(msgs[static_cast<std::size_t>(i)].msg_hdr);

    if (parsed.overflow) {
      // Cumulative and 32-bit-wrapping. The delta is what matters, and the very
      // first value is a baseline, not a burst of loss.
      if (overflow_primed_) {
        stats_.kernel_drops += static_cast<std::uint32_t>(*parsed.overflow - last_overflow_);
      }
      last_overflow_   = *parsed.overflow;
      overflow_primed_ = true;
    }

    auto len = static_cast<std::size_t>(msgs[static_cast<std::size_t>(i)].msg_len);
    if ((msgs[static_cast<std::size_t>(i)].msg_hdr.msg_flags & MSG_TRUNC) != 0) {
      // The datagram was larger than kMaxDatagramBytes. Truncating it would
      // hand a corrupt packet to the framer, which would then blame the venue,
      // so it is counted and dropped instead.
      ++stats_.truncated;
      continue;
    }
    len = std::min(len, kMaxDatagramBytes);

    batch_.push_back(Received{
        .offset = static_cast<std::size_t>(i) * kMaxDatagramBytes,
        .len    = len,
        .rx_ts  = parsed.rx_ts.value_or(now_realtime_ns()),
        .line   = sock.line,
        .kind   = sock.kind,
    });
  }

  stats_.datagrams += batch_.size();
  return batch_.size();
}

std::optional<RawPacket> UdpTransport::poll() noexcept {
  if (batch_pos_ < batch_.size()) {
    const auto& entry = batch_[batch_pos_++];
    return RawPacket{
        .bytes = std::span<const std::byte>{storage_.data() + entry.offset, entry.len},
        .rx_ts = entry.rx_ts,
        .line  = entry.line,
        .kind  = entry.kind,
    };
  }

  // Round-robin so one busy channel cannot starve the others. Starting from
  // next_socket_ rather than 0 is what makes that true across calls.
  for (std::size_t attempt = 0; attempt < sockets_.size(); ++attempt) {
    const auto index = (next_socket_ + attempt) % sockets_.size();
    if (receive_batch(index) > 0) {
      next_socket_ = (index + 1) % sockets_.size();
      return poll();
    }
  }

  batch_.clear();
  batch_pos_ = 0;
  return std::nullopt;
}

bool UdpTransport::wait(int timeout_ms) noexcept {
  if (batch_pos_ < batch_.size()) return true;

  std::vector<pollfd> fds(sockets_.size());
  for (std::size_t i = 0; i < sockets_.size(); ++i) {
    fds[i].fd     = sockets_[i].fd;
    fds[i].events = POLLIN;
  }
  return ::poll(fds.data(), fds.size(), timeout_ms) > 0;
}

}  // namespace rfeed

#endif  // __linux__
