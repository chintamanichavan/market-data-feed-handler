#pragma once

#include <optional>
#include <string>
#include <utility>

#include "rfeed/channel_config.hpp"
#include "rfeed/pcap.hpp"
#include "rfeed/transport.hpp"

namespace rfeed {

// Deterministic, file-backed transport. Demultiplexes a single capture — which
// is what one tcpdump run against a venue actually produces — into per-channel,
// per-line packets using the destination group and port.
//
// Datagrams whose destination matches no configured connection are counted and
// skipped, not treated as an error: real captures contain other traffic.
class PcapTransport {
 public:
  [[nodiscard]] static std::expected<PcapTransport, PcapError> open(const std::string& path,
                                                                    ChannelConfig config) {
    auto reader = PcapReader::open(path);
    if (!reader) return std::unexpected{reader.error()};
    return PcapTransport{std::move(*reader), std::move(config)};
  }

  [[nodiscard]] std::optional<RawPacket> poll() noexcept {
    while (auto dgram = reader_.next()) {
      const auto conn = config_.classify(dgram->dst_addr, dgram->dst_port);
      if (!conn) {
        ++unmatched_;
        continue;
      }
      ++delivered_;
      return RawPacket{
          .bytes = dgram->payload,
          .rx_ts = dgram->rx_ts,
          .line  = conn->line,
          .kind  = conn->kind,
      };
    }
    exhausted_ = true;
    return std::nullopt;
  }

  [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }

  void rewind() noexcept {
    reader_.rewind();
    exhausted_ = false;
    delivered_ = 0;
    unmatched_ = 0;
  }

  [[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_; }
  [[nodiscard]] std::uint64_t unmatched() const noexcept { return unmatched_; }
  [[nodiscard]] const ChannelConfig& config() const noexcept { return config_; }

 private:
  PcapTransport(PcapReader reader, ChannelConfig config)
      : reader_{std::move(reader)}, config_{std::move(config)} {}

  PcapReader    reader_;
  ChannelConfig config_;
  bool          exhausted_{false};
  std::uint64_t delivered_{0};
  std::uint64_t unmatched_{0};
};

static_assert(Transport<PcapTransport>);

}  // namespace rfeed
