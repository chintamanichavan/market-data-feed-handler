#include "rfeed/replay_client.hpp"

#ifndef __linux__

namespace rfeed {
ReplaySocket::~ReplaySocket() = default;
ReplaySocket::ReplaySocket(ReplaySocket&&) noexcept = default;
ReplaySocket& ReplaySocket::operator=(ReplaySocket&&) noexcept = default;
bool ReplaySocket::start_connect(std::uint32_t, std::uint16_t) noexcept { return false; }
int  ReplaySocket::poll_connect() noexcept { return 1; }
std::ptrdiff_t ReplaySocket::send(std::span<const std::byte>) noexcept { return -1; }
std::ptrdiff_t ReplaySocket::receive(std::span<std::byte>) noexcept { return -1; }
void ReplaySocket::close() noexcept {}
}  // namespace rfeed

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace rfeed {

ReplaySocket::~ReplaySocket() { close(); }

ReplaySocket::ReplaySocket(ReplaySocket&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }

ReplaySocket& ReplaySocket::operator=(ReplaySocket&& other) noexcept {
  if (this == &other) return *this;
  close();
  fd_       = other.fd_;
  other.fd_ = -1;
  return *this;
}

void ReplaySocket::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool ReplaySocket::start_connect(std::uint32_t addr, std::uint16_t port) noexcept {
  close();
  fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd_ < 0) return false;

  // The response is a burst of small records. Nagle would coalesce our request
  // with nothing and simply add a round trip to a recovery that is already the
  // slow path.
  const int on = 1;
  (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

  sockaddr_in dst{};
  dst.sin_family      = AF_INET;
  dst.sin_port        = ::htons(port);
  dst.sin_addr.s_addr = ::htonl(addr);

  if (::connect(fd_, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) == 0) return true;
  // EINPROGRESS is the expected answer for a non-blocking connect, not an error.
  if (errno == EINPROGRESS) return true;
  close();
  return false;
}

int ReplaySocket::poll_connect() noexcept {
  if (fd_ < 0) return 1;
  int       err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return 1;
  if (err == EINPROGRESS || err == EALREADY) return -1;
  if (err != 0) return 1;

  // SO_ERROR is 0 both while the handshake is still outstanding and once it has
  // succeeded, so it cannot settle the question on its own. getpeername does.
  sockaddr_in peer{};
  socklen_t   peer_len = sizeof(peer);
  if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len) != 0) {
    return errno == ENOTCONN ? -1 : 1;
  }
  return 0;
}

std::ptrdiff_t ReplaySocket::send(std::span<const std::byte> bytes) noexcept {
  if (fd_ < 0) return -1;
  // MSG_NOSIGNAL: a venue that closes the connection must surface as an error
  // return, never as SIGPIPE killing the whole handler.
  const auto n = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
  if (n >= 0) return n;
  return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

std::ptrdiff_t ReplaySocket::receive(std::span<std::byte> out) noexcept {
  if (fd_ < 0) return -1;
  const auto n = ::recv(fd_, out.data(), out.size(), 0);
  if (n > 0) return n;
  if (n == 0) return -2;  // orderly shutdown
  return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

}  // namespace rfeed

#endif  // __linux__
