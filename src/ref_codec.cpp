#include <cstdint>
#include <expected>

#include "rfeed/byte_reader.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"
#include "ref_venue.hpp"

namespace rfeed::venue_ref {
namespace {

// Every field read is checked. `RFEED_READ(dst, T)` keeps the failure path
// uniform without hiding it — the alternative is fifty hand-written early
// returns that all look slightly different from one another.
#define RFEED_READ(name, type, reader)              \
  const auto name = (reader).read<type>();          \
  if (!(name)) return std::unexpected{(name).error()}

}  // namespace

std::expected<IncrementalHeader, DecodeError> RefCodec::incremental_header(ByteReader& in) noexcept {
  RFEED_READ(transact_time, std::uint64_t, in);
  RFEED_READ(event, std::uint8_t, in);

  return IncrementalHeader{
      .transact_time = static_cast<Timestamp>(*transact_time),
      .end_of_event  = (*event & kEventEndOfEventBit) != 0,
  };
}

std::expected<MdEntry, DecodeError> RefCodec::md_entry(ByteReader& in) noexcept {
  RFEED_READ(px, std::int64_t, in);
  RFEED_READ(size, std::int64_t, in);
  RFEED_READ(security, std::int32_t, in);
  RFEED_READ(rpt_seq, std::uint32_t, in);
  RFEED_READ(num_orders, std::int32_t, in);
  RFEED_READ(price_level, std::uint8_t, in);
  RFEED_READ(action, std::uint8_t, in);
  RFEED_READ(type, std::uint8_t, in);

  return MdEntry{
      .px          = Mantissa{*px},
      .size        = *size,
      .security    = *security,
      .rpt_seq     = *rpt_seq,
      .num_orders  = *num_orders,
      .price_level = *price_level,
      .action      = to_update_action(*action),
      .type        = to_entry_type(*type),
  };
}

std::expected<SnapshotHeader, DecodeError> RefCodec::snapshot_header(ByteReader& in) noexcept {
  RFEED_READ(last_seq, std::uint32_t, in);
  RFEED_READ(tot_num_reports, std::uint32_t, in);
  RFEED_READ(security, std::int32_t, in);
  RFEED_READ(rpt_seq, std::uint32_t, in);
  RFEED_READ(transact_time, std::uint64_t, in);
  RFEED_READ(status, std::uint8_t, in);

  return SnapshotHeader{
      .last_msg_seq_num_processed = *last_seq,
      .tot_num_reports            = *tot_num_reports,
      .security                   = *security,
      .rpt_seq                    = *rpt_seq,
      .transact_time              = static_cast<Timestamp>(*transact_time),
      .status                     = to_trading_status(*status),
  };
}

std::expected<SnapshotEntry, DecodeError> RefCodec::snapshot_entry(ByteReader& in) noexcept {
  RFEED_READ(px, std::int64_t, in);
  RFEED_READ(size, std::int64_t, in);
  RFEED_READ(num_orders, std::int32_t, in);
  RFEED_READ(price_level, std::uint8_t, in);
  RFEED_READ(type, std::uint8_t, in);

  return SnapshotEntry{
      .px          = Mantissa{*px},
      .size        = *size,
      .num_orders  = *num_orders,
      .price_level = *price_level,
      .type        = to_entry_type(*type),
  };
}

std::expected<InstrumentDefinition, DecodeError> RefCodec::instrument_definition(
    ByteReader& in) noexcept {
  RFEED_READ(security, std::int32_t, in);

  const auto symbol = in.read_bytes(kMaxSymbolLen);
  if (!symbol) return std::unexpected{symbol.error()};

  RFEED_READ(min_price_increment, std::int64_t, in);
  RFEED_READ(tot_num_reports, std::uint32_t, in);
  RFEED_READ(market_depth, std::uint8_t, in);
  RFEED_READ(sub_tick_divisor, std::uint8_t, in);

  InstrumentDefinition def{};
  def.security = *security;
  for (std::size_t i = 0; i < kMaxSymbolLen; ++i) {
    def.symbol[i] = static_cast<char>(std::to_integer<std::uint8_t>((*symbol)[i]));
  }
  def.min_price_increment = Mantissa{*min_price_increment};
  def.tot_num_reports     = *tot_num_reports;
  def.market_depth        = *market_depth;
  // A zero divisor on the wire would make tick conversion divide by zero.
  def.sub_tick_divisor = (*sub_tick_divisor == 0) ? std::uint8_t{1} : *sub_tick_divisor;
  return def;
}

std::expected<SecurityStatusMsg, DecodeError> RefCodec::security_status(ByteReader& in) noexcept {
  RFEED_READ(security, std::int32_t, in);
  RFEED_READ(transact_time, std::uint64_t, in);
  RFEED_READ(status, std::uint8_t, in);

  return SecurityStatusMsg{
      .security      = *security,
      .transact_time = static_cast<Timestamp>(*transact_time),
      .status        = to_trading_status(*status),
  };
}

std::expected<ChannelResetMsg, DecodeError> RefCodec::channel_reset(ByteReader& in) noexcept {
  RFEED_READ(transact_time, std::uint64_t, in);
  RFEED_READ(kind, std::uint8_t, in);

  return ChannelResetMsg{
      .transact_time = static_cast<Timestamp>(*transact_time),
      .kind          = (*kind == 1) ? ResetKind::PeriodStart : ResetKind::Intraday,
  };
}

#undef RFEED_READ

}  // namespace rfeed::venue_ref
