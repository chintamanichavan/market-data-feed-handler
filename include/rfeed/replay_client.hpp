#pragma once

// Non-blocking TCP replay client.
//
// Structured as a state machine driven by poll(), not as a blocking call, for
// one reason: a replay is requested precisely when the feed is unhealthy, and
// blocking here would stop the handler from processing the live multicast that
// is still arriving. The cure would cause the disease.
//
// Rate limits are enforced by ReplayLimiter before a socket is touched, and a
// refusal is never retried automatically — RecoveryFsm::on_replay_failed falls
// back to a snapshot, which costs the venue nothing.

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include "rfeed/channel_config.hpp"
#include "rfeed/line_arbiter.hpp"
#include "rfeed/replay.hpp"
#include "rfeed/transport.hpp"
#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed {

enum class ReplayPhase : std::uint8_t {
  Idle,        // no request outstanding
  Connecting,
  Sending,
  Receiving,
  Complete,    // packets are ready to drain
  Failed,
};

[[nodiscard]] constexpr std::string_view to_string(ReplayPhase p) noexcept {
  switch (p) {
    case ReplayPhase::Idle:       return "Idle";
    case ReplayPhase::Connecting: return "Connecting";
    case ReplayPhase::Sending:    return "Sending";
    case ReplayPhase::Receiving:  return "Receiving";
    case ReplayPhase::Complete:   return "Complete";
    case ReplayPhase::Failed:     return "Failed";
  }
  return "Unknown";
}

enum class ReplayFailure : std::uint8_t {
  None,
  Connect,
  Send,
  Receive,
  ProtocolViolation,
  Timeout,
  RefusedByVenue,
  ShortResponse,  // venue returned fewer packets than the range asked for
};

[[nodiscard]] constexpr std::string_view to_string(ReplayFailure f) noexcept {
  switch (f) {
    case ReplayFailure::None:              return "None";
    case ReplayFailure::Connect:           return "Connect";
    case ReplayFailure::Send:              return "Send";
    case ReplayFailure::Receive:           return "Receive";
    case ReplayFailure::ProtocolViolation: return "ProtocolViolation";
    case ReplayFailure::Timeout:           return "Timeout";
    case ReplayFailure::RefusedByVenue:    return "RefusedByVenue";
    case ReplayFailure::ShortResponse:     return "ShortResponse";
  }
  return "Unknown";
}

struct ReplayClientStats {
  std::uint64_t requests{0};
  std::uint64_t completed{0};
  std::uint64_t failures{0};
  std::uint64_t packets_recovered{0};
  std::uint64_t bytes_received{0};
};

// The socket half, kept free of venue detail. Sequencing, limits and protocol
// all live above it.
class ReplaySocket {
 public:
  ReplaySocket() = default;
  ~ReplaySocket();
  ReplaySocket(ReplaySocket&&) noexcept;
  ReplaySocket& operator=(ReplaySocket&&) noexcept;
  ReplaySocket(const ReplaySocket&)            = delete;
  ReplaySocket& operator=(const ReplaySocket&) = delete;

  // Starts a non-blocking connect. False means the socket could not be created.
  [[nodiscard]] bool start_connect(std::uint32_t addr, std::uint16_t port) noexcept;

  // -1 still connecting, 0 connected, 1 failed.
  [[nodiscard]] int poll_connect() noexcept;

  // Bytes written, or -1 on a real error (EAGAIN reports 0).
  [[nodiscard]] std::ptrdiff_t send(std::span<const std::byte> bytes) noexcept;

  // Bytes read; 0 for EAGAIN, -1 on error, -2 on a clean peer close.
  [[nodiscard]] std::ptrdiff_t receive(std::span<std::byte> out) noexcept;

  void close() noexcept;
  [[nodiscard]] bool open() const noexcept { return fd_ >= 0; }

 private:
  int fd_{-1};
};

// Templated on the venue for its limits, and on the protocol for its framing.
template <VenuePolicy V, class Protocol>
class ReplayClient {
 public:
  ReplayClient(Endpoint endpoint, ChannelId channel, Timestamp timeout_ns = 5'000'000'000)
      : endpoint_{endpoint}, channel_{channel}, timeout_ns_{timeout_ns} {}

  // Asks for a range. Rejected without touching the network when the venue's
  // budget does not allow it — which is a normal outcome, not an error: the
  // caller falls back to a snapshot.
  [[nodiscard]] std::expected<void, ReplayReject> request(const GapRange& gap,
                                                          Timestamp now) noexcept {
    if (phase_ != ReplayPhase::Idle && phase_ != ReplayPhase::Complete &&
        phase_ != ReplayPhase::Failed) {
      return std::unexpected{ReplayReject::TooManyInFlight};
    }
    if (const auto ok = limiter_.acquire(gap, now); !ok) return ok;

    reset_session();
    gap_      = gap;
    deadline_ = now + timeout_ns_;
    ++stats_.requests;

    Protocol::encode_request(out_, channel_, gap);

    if (!socket_.start_connect(endpoint_.group, endpoint_.port)) {
      fail(ReplayFailure::Connect);
      return {};
    }
    phase_ = ReplayPhase::Connecting;
    return {};
  }

  // Advances the state machine. Must be called even when idle, because the
  // timeout is only detectable from here.
  void poll(Timestamp now) noexcept {
    if (phase_ == ReplayPhase::Idle || phase_ == ReplayPhase::Complete ||
        phase_ == ReplayPhase::Failed) {
      return;
    }
    if (now >= deadline_) {
      fail(ReplayFailure::Timeout);
      return;
    }

    switch (phase_) {
      case ReplayPhase::Connecting: {
        const auto status = socket_.poll_connect();
        if (status < 0) return;  // still in progress
        if (status > 0) {
          fail(ReplayFailure::Connect);
          return;
        }
        phase_ = ReplayPhase::Sending;
        [[fallthrough]];
      }

      case ReplayPhase::Sending: {
        while (sent_ < out_.size()) {
          const auto n = socket_.send(std::span<const std::byte>{out_}.subspan(sent_));
          if (n < 0) {
            fail(ReplayFailure::Send);
            return;
          }
          if (n == 0) return;  // would block; try again next poll
          sent_ += static_cast<std::size_t>(n);
        }
        phase_ = ReplayPhase::Receiving;
        [[fallthrough]];
      }

      case ReplayPhase::Receiving: {
        std::array<std::byte, 16384> scratch{};
        while (true) {
          const auto n = socket_.receive(scratch);
          if (n == 0) return;  // would block
          if (n == -2) {
            // Peer closed. Complete only if the response was whole; a truncated
            // body must not be mistaken for a successful recovery.
            finish_on_close();
            return;
          }
          if (n < 0) {
            fail(ReplayFailure::Receive);
            return;
          }
          stats_.bytes_received += static_cast<std::uint64_t>(n);
          if (!parser_.consume(std::span<const std::byte>{scratch.data(),
                                                          static_cast<std::size_t>(n)})) {
            fail(ReplayFailure::ProtocolViolation);
            return;
          }
          if (parser_.header_seen() && parser_.status() != Protocol::kOkStatus) {
            fail(ReplayFailure::RefusedByVenue);
            return;
          }
          if (parser_.complete()) {
            complete();
            return;
          }
        }
      }

      case ReplayPhase::Idle:
      case ReplayPhase::Complete:
      case ReplayPhase::Failed:
        return;
    }
  }

  // Hands over the recovered datagrams and returns to Idle. Each is exactly the
  // datagram the live feed would have carried, so it re-enters the pipeline
  // through the same decode path.
  template <class Fn>
  void drain(Fn&& fn) {
    if (phase_ != ReplayPhase::Complete) return;
    for (const auto& packet : parser_.packets()) {
      fn(std::span<const std::byte>{packet.data(), packet.size()});
    }
    reset_session();
    phase_ = ReplayPhase::Idle;
  }

  [[nodiscard]] ReplayPhase phase() const noexcept { return phase_; }
  [[nodiscard]] ReplayFailure failure() const noexcept { return failure_; }
  [[nodiscard]] bool busy() const noexcept {
    return phase_ == ReplayPhase::Connecting || phase_ == ReplayPhase::Sending ||
           phase_ == ReplayPhase::Receiving;
  }
  [[nodiscard]] const ReplayClientStats& stats() const noexcept { return stats_; }
  [[nodiscard]] ReplayLimiter<V>& limiter() noexcept { return limiter_; }
  [[nodiscard]] const GapRange& gap() const noexcept { return gap_; }
  [[nodiscard]] std::size_t recovered() const noexcept { return parser_.packets().size(); }

  // Clears a terminal state so the next request can start.
  void acknowledge_failure() noexcept {
    if (phase_ == ReplayPhase::Failed) {
      phase_   = ReplayPhase::Idle;
      failure_ = ReplayFailure::None;
    }
  }

 private:
  void reset_session() noexcept {
    parser_.reset();
    out_.clear();
    sent_ = 0;
    socket_.close();
  }

  void complete() noexcept {
    socket_.close();
    // A short response is a failure even though every byte parsed cleanly.
    // Applying a partial range would leave a hole the handler believes it filled.
    if (parser_.packets().size() < gap_.count()) {
      ++stats_.failures;
      failure_ = ReplayFailure::ShortResponse;
      phase_   = ReplayPhase::Failed;
      limiter_.release();
      return;
    }
    ++stats_.completed;
    stats_.packets_recovered += parser_.packets().size();
    failure_ = ReplayFailure::None;
    phase_   = ReplayPhase::Complete;
    limiter_.release();
  }

  void finish_on_close() noexcept {
    if (parser_.complete()) {
      complete();
      return;
    }
    fail(ReplayFailure::ShortResponse);
  }

  void fail(ReplayFailure reason) noexcept {
    socket_.close();
    ++stats_.failures;
    failure_ = reason;
    phase_   = ReplayPhase::Failed;
    limiter_.release();
  }

  Endpoint          endpoint_{};
  ChannelId         channel_{};
  Timestamp         timeout_ns_{};
  ReplaySocket      socket_{};
  Protocol          parser_{};
  ReplayLimiter<V>  limiter_{};
  ReplayClientStats stats_{};
  std::vector<std::byte> out_{};
  std::size_t       sent_{0};
  GapRange          gap_{};
  Timestamp         deadline_{0};
  ReplayPhase       phase_{ReplayPhase::Idle};
  ReplayFailure     failure_{ReplayFailure::None};
};

}  // namespace rfeed
