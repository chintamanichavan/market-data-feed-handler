#include "rfeed/packet.hpp"

#include <cstdint>
#include <expected>

#include "rfeed/byte_reader.hpp"
#include "rfeed/sbe_header.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

std::expected<SbeMessageHeader, DecodeError> SbeMessageHeader::decode(ByteReader& in) noexcept {
  const auto block_length = in.read<std::uint16_t>();
  if (!block_length) return std::unexpected{block_length.error()};
  const auto template_id = in.read<std::uint16_t>();
  if (!template_id) return std::unexpected{template_id.error()};
  const auto schema_id = in.read<std::uint16_t>();
  if (!schema_id) return std::unexpected{schema_id.error()};
  const auto version = in.read<std::uint16_t>();
  if (!version) return std::unexpected{version.error()};

  return SbeMessageHeader{*block_length, *template_id, *schema_id, *version};
}

std::expected<PacketHeader, DecodeError> PacketHeader::decode(ByteReader& in) noexcept {
  const auto seq = in.read<std::uint32_t>();
  if (!seq) return std::unexpected{seq.error()};
  const auto sending_time = in.read<std::uint64_t>();
  if (!sending_time) return std::unexpected{sending_time.error()};

  return PacketHeader{*seq, static_cast<Timestamp>(*sending_time)};
}

std::expected<MessageView, DecodeError> MessageFramer::next() noexcept {
  if (failed_) return std::unexpected{error_};
  if (reader_.empty()) return std::unexpected{DecodeError::EndOfPacket};

  const auto fail = [this](DecodeError e) {
    failed_ = true;
    error_  = e;
    return std::unexpected{e};
  };

  // A trailing byte after the last message is a framing fault, not a clean end.
  const auto msg_size = reader_.read<std::uint16_t>();
  if (!msg_size) return fail(msg_size.error());

  const auto size = static_cast<std::size_t>(*msg_size);

  // Catches MsgSize == 0, which would otherwise advance the cursor by nothing
  // and spin forever.
  if (size < SbeMessageHeader::kEncodedSize) return fail(DecodeError::BadMessageSize);
  if (size > reader_.remaining()) return fail(DecodeError::MessageOverrunsPacket);

  const auto framed = reader_.read_bytes(size);
  if (!framed) return fail(framed.error());

  ByteReader msg{*framed};
  const auto hdr = SbeMessageHeader::decode(msg);
  if (!hdr) return fail(hdr.error());

  // The root block must fit inside what the framing claimed. Groups may follow
  // it; validating those is the generated codec's job, over this bounded span.
  const auto body = msg.rest();
  if (static_cast<std::size_t>(hdr->block_length) > body.size()) {
    return fail(DecodeError::BlockLengthOverrunsMessage);
  }

  return MessageView{*hdr, body};
}

std::expected<PacketView, DecodeError> PacketView::decode(
    std::span<const std::byte> datagram) noexcept {
  ByteReader in{datagram};
  const auto header = PacketHeader::decode(in);
  if (!header) return std::unexpected{header.error()};

  return PacketView{*header, in.rest()};
}

}  // namespace rfeed
