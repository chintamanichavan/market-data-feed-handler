#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

#include "rfeed/channel_config.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

// Upper bound on a single MDP datagram. Sized for a 1500-byte MTU with room to
// spare; a venue running jumbo frames needs this raised, and the arbiter's
// buffer footprint scales with it.
inline constexpr std::size_t kMaxDatagramBytes = 2048;

// One datagram as delivered by a transport.
//
// `bytes` is a view into the transport's receive buffer and is invalidated by
// the next poll(). Anything that needs to outlive the call must copy — which is
// exactly why LineArbiter copies only on the out-of-order path.
struct RawPacket {
  std::span<const std::byte> bytes{};
  Timestamp                  rx_ts{};  // hardware timestamp where available
  LineId                     line{LineId::A};
  ChannelKind                kind{ChannelKind::Incremental};
};

// The seam that keeps the network out of Phases 1–8.
//
// PcapTransport and UdpTransport both satisfy this, so the entire pipeline —
// framing, decode, arbitration, book building, recovery — is developed and
// tested against files and only later pointed at a socket. AF_XDP drops in the
// same way, which is why no kernel-bypass code appears before v2.
template <class T>
concept Transport = requires(T& t) {
  { t.poll() } -> std::same_as<std::optional<RawPacket>>;
  { t.exhausted() } -> std::same_as<bool>;
};

}  // namespace rfeed
