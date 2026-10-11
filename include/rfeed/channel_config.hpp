#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rfeed/types.hpp"

namespace rfeed {

// Venue-neutral connection model. Every venue publishes this information in its
// own format — CME ships a config.xml with <connection> elements; another venue
// may ship JSON, CSV, or a PDF table. A per-venue ConfigSource parses whatever
// arrives into exactly this, and nothing downstream knows the difference.
enum class ChannelKind : std::uint8_t {
  Incremental,           // CME feed-type "I"
  Snapshot,              // "S" — the continuously looping recovery channel
  InstrumentDefinition,  // "N" — the definition replay loop
  HistoricalReplay,      // "H" — TCP recovery service
};

[[nodiscard]] constexpr std::string_view to_string(ChannelKind k) noexcept {
  switch (k) {
    case ChannelKind::Incremental:          return "Incremental";
    case ChannelKind::Snapshot:             return "Snapshot";
    case ChannelKind::InstrumentDefinition: return "InstrumentDefinition";
    case ChannelKind::HistoricalReplay:     return "HistoricalReplay";
  }
  return "Unknown";
}

struct Endpoint {
  std::uint32_t group{};   // multicast group, host byte order
  std::uint16_t port{};
  std::uint32_t source{};  // source-specific multicast; 0 = any

  [[nodiscard]] constexpr bool valid() const noexcept { return group != 0 && port != 0; }
  auto operator<=>(const Endpoint&) const = default;
};

struct ConnectionConfig {
  ChannelKind kind{ChannelKind::Incremental};
  LineId      line{LineId::A};
  Endpoint    endpoint{};
};

struct ChannelConfig {
  ChannelId                     id{};
  std::string                   name{};
  std::vector<ConnectionConfig> connections{};

  [[nodiscard]] std::optional<Endpoint> endpoint_for(ChannelKind kind, LineId line) const {
    for (const auto& c : connections) {
      if (c.kind == kind && c.line == line) return c.endpoint;
    }
    return std::nullopt;
  }

  // Which channel/line a datagram belongs to, resolved by destination. This is
  // how a single capture containing every channel gets demultiplexed.
  [[nodiscard]] std::optional<ConnectionConfig> classify(std::uint32_t dst_addr,
                                                         std::uint16_t dst_port) const {
    for (const auto& c : connections) {
      if (c.endpoint.group == dst_addr && c.endpoint.port == dst_port) return c;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool has(ChannelKind kind) const {
    for (const auto& c : connections) {
      if (c.kind == kind) return true;
    }
    return false;
  }
};

}  // namespace rfeed
