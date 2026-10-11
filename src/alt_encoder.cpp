#include "alt_encoder.hpp"

#include <cstdint>

#include "alt_venue.hpp"
#include "rfeed/sbe_header.hpp"

namespace rfeed::venue_alt {
namespace {

template <class T>
void put(std::vector<std::byte>& out, T value) {
  using U      = std::make_unsigned_t<T>;
  const auto u = static_cast<U>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((u >> (8U * i)) & 0xFFU)));
  }
}

void pad_to(std::vector<std::byte>& out, std::size_t start, std::uint16_t block_length) {
  while (out.size() - start < block_length) out.push_back(std::byte{0});
}

std::size_t open_message(std::vector<std::byte>& out, std::uint16_t template_id,
                         std::uint16_t block_length) {
  const auto size_off = out.size();
  put<std::uint16_t>(out, 0);  // MsgSize, backfilled on close
  put<std::uint16_t>(out, block_length);
  put<std::uint16_t>(out, template_id);
  put<std::uint16_t>(out, AltVenue::kSchemaId);
  put<std::uint16_t>(out, AltVenue::kMinSupportedVersion);
  return size_off;
}

void close_message(std::vector<std::byte>& out, std::size_t size_off) {
  const auto size = static_cast<std::uint16_t>(out.size() - size_off - 2);
  out[size_off]     = static_cast<std::byte>(static_cast<std::uint8_t>(size & 0xFFU));
  out[size_off + 1] = static_cast<std::byte>(static_cast<std::uint8_t>((size >> 8) & 0xFFU));
}

// The whole reason this venue exists: a uint16 numInGroup. One byte's
// difference, and every repeating group after the first misparses silently if
// the core ever assumes the other width.
void put_group_header(std::vector<std::byte>& out, std::uint16_t block_length, std::size_t count) {
  put<std::uint16_t>(out, block_length);
  static_assert(AltVenue::kGroupCountBytes == 2, "encoder follows the venue's numInGroup width");
  put<std::uint16_t>(out, static_cast<std::uint16_t>(count));
}

}  // namespace

Encoder::Encoder(SeqNum seq, Timestamp sending_time) {
  put<std::uint32_t>(bytes_, seq);
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(sending_time));
}

Encoder& Encoder::incremental(const IncrementalHeader& header, std::span<const MdEntry> entries) {
  const auto size_off    = open_message(bytes_, kTplIncrementalBook, kBlkIncremental);
  const auto block_start = bytes_.size();

  put<std::uint8_t>(bytes_, header.end_of_event ? kEventEndOfEventBit : std::uint8_t{0});
  put<std::uint8_t>(bytes_, 0);
  put<std::uint16_t>(bytes_, 0);
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(header.transact_time));
  pad_to(bytes_, block_start, kBlkIncremental);

  put_group_header(bytes_, kBlkMdEntry, entries.size());
  for (const auto& e : entries) {
    const auto entry_start = bytes_.size();
    put<std::int32_t>(bytes_, e.security);
    put<std::uint32_t>(bytes_, e.rpt_seq);
    put<std::int64_t>(bytes_, e.px.raw);
    put<std::int64_t>(bytes_, e.size);
    put<std::int32_t>(bytes_, e.num_orders);
    put<std::uint8_t>(bytes_, e.price_level);
    put<std::uint8_t>(bytes_, from_update_action(e.action));
    put<std::uint8_t>(bytes_, from_entry_type(e.type));
    pad_to(bytes_, entry_start, kBlkMdEntry);
  }

  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

Encoder& Encoder::snapshot(const SnapshotHeader& header, std::span<const SnapshotEntry> entries) {
  const auto size_off    = open_message(bytes_, kTplSnapshot, kBlkSnapshot);
  const auto block_start = bytes_.size();

  put<std::int32_t>(bytes_, header.security);
  put<std::uint32_t>(bytes_, header.rpt_seq);
  put<std::uint32_t>(bytes_, header.last_msg_seq_num_processed);
  put<std::uint32_t>(bytes_, header.tot_num_reports);
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(header.transact_time));
  put<std::uint8_t>(bytes_, from_trading_status(header.status));
  pad_to(bytes_, block_start, kBlkSnapshot);

  put_group_header(bytes_, kBlkSnapshotEntry, entries.size());
  for (const auto& e : entries) {
    const auto entry_start = bytes_.size();
    put<std::uint8_t>(bytes_, e.price_level);
    put<std::uint8_t>(bytes_, from_entry_type(e.type));
    put<std::uint16_t>(bytes_, 0);
    put<std::int32_t>(bytes_, e.num_orders);
    put<std::int64_t>(bytes_, e.px.raw);
    put<std::int64_t>(bytes_, e.size);
    pad_to(bytes_, entry_start, kBlkSnapshotEntry);
  }

  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

Encoder& Encoder::instrument_definition(const InstrumentDefinition& def) {
  const auto size_off    = open_message(bytes_, kTplInstrumentDef, kBlkInstrumentDef);
  const auto block_start = bytes_.size();

  put<std::int32_t>(bytes_, def.security);
  put<std::int64_t>(bytes_, def.min_price_increment.raw);
  put<std::uint32_t>(bytes_, def.tot_num_reports);
  put<std::uint8_t>(bytes_, def.market_depth);
  put<std::uint8_t>(bytes_, def.sub_tick_divisor);
  for (std::size_t i = 0; i < kMaxSymbolLen; ++i) {
    bytes_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(def.symbol[i])));
  }
  pad_to(bytes_, block_start, kBlkInstrumentDef);

  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

Encoder& Encoder::security_status(const SecurityStatusMsg& msg) {
  const auto size_off    = open_message(bytes_, kTplSecurityStatus, kBlkSecurityStatus);
  const auto block_start = bytes_.size();

  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(msg.transact_time));
  put<std::int32_t>(bytes_, msg.security);
  put<std::uint8_t>(bytes_, from_trading_status(msg.status));
  pad_to(bytes_, block_start, kBlkSecurityStatus);

  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

Encoder& Encoder::channel_reset(const ChannelResetMsg& msg) {
  const auto size_off    = open_message(bytes_, kTplChannelReset, kBlkChannelReset);
  const auto block_start = bytes_.size();

  put<std::uint8_t>(bytes_, msg.kind == ResetKind::PeriodStart ? std::uint8_t{1} : std::uint8_t{0});
  put<std::uint8_t>(bytes_, 0);
  put<std::uint16_t>(bytes_, 0);
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(msg.transact_time));
  pad_to(bytes_, block_start, kBlkChannelReset);

  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

Encoder& Encoder::admin_heartbeat() {
  const auto size_off = open_message(bytes_, kTplHeartbeat, 0);
  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

}  // namespace rfeed::venue_alt
