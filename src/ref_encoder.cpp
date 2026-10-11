#include "ref_encoder.hpp"

#include <cstdint>

#include "ref_venue.hpp"
#include "rfeed/sbe_header.hpp"

namespace rfeed::venue_ref {
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

[[nodiscard]] std::uint8_t from_entry_type(EntryType t) noexcept {
  switch (t) {
    case EntryType::Bid:          return kEntryBid;
    case EntryType::Offer:        return kEntryOffer;
    case EntryType::Trade:        return kEntryTrade;
    case EntryType::ImpliedBid:   return kEntryImpliedBid;
    case EntryType::ImpliedOffer: return kEntryImpliedOffer;
    case EntryType::Unknown:      return 0xFF;
  }
  return 0xFF;
}

[[nodiscard]] std::uint8_t from_update_action(UpdateAction a) noexcept {
  switch (a) {
    case UpdateAction::New:        return 0;
    case UpdateAction::Change:     return 1;
    case UpdateAction::Delete:     return 2;
    case UpdateAction::DeleteThru: return 3;
    case UpdateAction::DeleteFrom: return 4;
    case UpdateAction::Overlay:    return 5;
    case UpdateAction::Unknown:    return 0xFF;
  }
  return 0xFF;
}

[[nodiscard]] std::uint8_t from_trading_status(TradingStatus s) noexcept {
  switch (s) {
    case TradingStatus::Halted:  return kStatusHalted;
    case TradingStatus::Closed:  return kStatusClosed;
    case TradingStatus::Open:    return kStatusOpen;
    case TradingStatus::Paused:  return kStatusPaused;
    case TradingStatus::PreOpen: return kStatusPreOpen;
    case TradingStatus::Unknown: return 0;
  }
  return 0;
}

}  // namespace

// Opens a message: writes MsgSize (backfilled on close) and the SBE header.
// Returns the offset of the MsgSize field.
static std::size_t open_message(std::vector<std::byte>& out, std::uint16_t template_id,
                                std::uint16_t block_length) {
  const auto size_off = out.size();
  put<std::uint16_t>(out, 0);  // placeholder
  put<std::uint16_t>(out, block_length);
  put<std::uint16_t>(out, template_id);
  put<std::uint16_t>(out, RefVenue::kSchemaId);
  put<std::uint16_t>(out, RefVenue::kMinSupportedVersion);
  return size_off;
}

static void close_message(std::vector<std::byte>& out, std::size_t size_off) {
  // MsgSize covers the SBE header and body, but not its own two bytes.
  const auto size = static_cast<std::uint16_t>(out.size() - size_off - 2);
  out[size_off]     = static_cast<std::byte>(static_cast<std::uint8_t>(size & 0xFFU));
  out[size_off + 1] = static_cast<std::byte>(static_cast<std::uint8_t>((size >> 8) & 0xFFU));
}

static void put_group_header(std::vector<std::byte>& out, std::uint16_t block_length,
                             std::size_t count) {
  put<std::uint16_t>(out, block_length);
  static_assert(RefVenue::kGroupCountBytes == 1, "encoder follows the venue's numInGroup width");
  put<std::uint8_t>(out, static_cast<std::uint8_t>(count));
}

Encoder::Encoder(SeqNum seq, Timestamp sending_time) {
  put<std::uint32_t>(bytes_, seq);
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(sending_time));
}

Encoder& Encoder::incremental(const IncrementalHeader& header, std::span<const MdEntry> entries) {
  const auto size_off = open_message(bytes_, kTplIncrementalBook, kBlkIncremental);

  const auto block_start = bytes_.size();
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(header.transact_time));
  put<std::uint8_t>(bytes_, header.end_of_event ? kEventEndOfEventBit : std::uint8_t{0});
  pad_to(bytes_, block_start, kBlkIncremental);

  put_group_header(bytes_, kBlkMdEntry, entries.size());
  for (const auto& e : entries) {
    const auto entry_start = bytes_.size();
    put<std::int64_t>(bytes_, e.px.raw);
    put<std::int64_t>(bytes_, e.size);
    put<std::int32_t>(bytes_, e.security);
    put<std::uint32_t>(bytes_, e.rpt_seq);
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
  const auto size_off = open_message(bytes_, kTplSnapshot, kBlkSnapshot);

  const auto block_start = bytes_.size();
  put<std::uint32_t>(bytes_, header.last_msg_seq_num_processed);
  put<std::uint32_t>(bytes_, header.tot_num_reports);
  put<std::int32_t>(bytes_, header.security);
  put<std::uint32_t>(bytes_, header.rpt_seq);
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(header.transact_time));
  put<std::uint8_t>(bytes_, from_trading_status(header.status));
  pad_to(bytes_, block_start, kBlkSnapshot);

  put_group_header(bytes_, kBlkSnapshotEntry, entries.size());
  for (const auto& e : entries) {
    const auto entry_start = bytes_.size();
    put<std::int64_t>(bytes_, e.px.raw);
    put<std::int64_t>(bytes_, e.size);
    put<std::int32_t>(bytes_, e.num_orders);
    put<std::uint8_t>(bytes_, e.price_level);
    put<std::uint8_t>(bytes_, from_entry_type(e.type));
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
  for (std::size_t i = 0; i < kMaxSymbolLen; ++i) {
    bytes_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(def.symbol[i])));
  }
  put<std::int64_t>(bytes_, def.min_price_increment.raw);
  put<std::uint32_t>(bytes_, def.tot_num_reports);
  put<std::uint8_t>(bytes_, def.market_depth);
  put<std::uint8_t>(bytes_, def.sub_tick_divisor);
  pad_to(bytes_, block_start, kBlkInstrumentDef);

  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

Encoder& Encoder::security_status(const SecurityStatusMsg& msg) {
  const auto size_off    = open_message(bytes_, kTplSecurityStatus, kBlkSecurityStatus);
  const auto block_start = bytes_.size();

  put<std::int32_t>(bytes_, msg.security);
  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(msg.transact_time));
  put<std::uint8_t>(bytes_, from_trading_status(msg.status));
  pad_to(bytes_, block_start, kBlkSecurityStatus);

  close_message(bytes_, size_off);
  ++messages_;
  return *this;
}

Encoder& Encoder::channel_reset(const ChannelResetMsg& msg) {
  const auto size_off    = open_message(bytes_, kTplChannelReset, kBlkChannelReset);
  const auto block_start = bytes_.size();

  put<std::uint64_t>(bytes_, static_cast<std::uint64_t>(msg.transact_time));
  put<std::uint8_t>(bytes_, msg.kind == ResetKind::PeriodStart ? std::uint8_t{1} : std::uint8_t{0});
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

std::vector<std::byte> encode_incremental_packet(SeqNum seq, Timestamp sending_time,
                                                 const IncrementalHeader& header,
                                                 std::span<const MdEntry> entries) {
  Encoder enc{seq, sending_time};
  enc.incremental(header, entries);
  const auto view = enc.view();
  return std::vector<std::byte>{view.begin(), view.end()};
}

}  // namespace rfeed::venue_ref
