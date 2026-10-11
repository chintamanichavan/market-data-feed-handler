#include <cstdint>
#include <expected>

#include "alt_venue.hpp"
#include "rfeed/byte_reader.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"

namespace rfeed::venue_alt {
namespace {

#define RFEED_READ(name, type, reader)     \
  const auto name = (reader).read<type>(); \
  if (!(name)) return std::unexpected{(name).error()}

#define RFEED_SKIP(reader, n)               \
  if (const auto skipped = (reader).skip(n); !skipped) return std::unexpected{skipped.error()}

}  // namespace

// Note the field ORDER throughout: nothing here matches venue_ref. A core that
// had quietly baked in one venue's layout would fail every one of these.

std::expected<IncrementalHeader, DecodeError> AltCodec::incremental_header(
    ByteReader& in) noexcept {
  RFEED_READ(flags, std::uint8_t, in);  // flags first here, last on venue_ref
  RFEED_SKIP(in, 3);
  RFEED_READ(transact_time, std::uint64_t, in);

  return IncrementalHeader{
      .transact_time = static_cast<Timestamp>(*transact_time),
      .end_of_event  = (*flags & kEventEndOfEventBit) != 0,
  };
}

std::expected<MdEntry, DecodeError> AltCodec::md_entry(ByteReader& in) noexcept {
  RFEED_READ(security, std::int32_t, in);
  RFEED_READ(rpt_seq, std::uint32_t, in);
  RFEED_READ(px, std::int64_t, in);
  RFEED_READ(size, std::int64_t, in);
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

std::expected<SnapshotHeader, DecodeError> AltCodec::snapshot_header(ByteReader& in) noexcept {
  RFEED_READ(security, std::int32_t, in);
  RFEED_READ(rpt_seq, std::uint32_t, in);
  RFEED_READ(last_seq, std::uint32_t, in);
  RFEED_READ(tot_num_reports, std::uint32_t, in);
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

std::expected<SnapshotEntry, DecodeError> AltCodec::snapshot_entry(ByteReader& in) noexcept {
  RFEED_READ(price_level, std::uint8_t, in);
  RFEED_READ(type, std::uint8_t, in);
  RFEED_SKIP(in, 2);
  RFEED_READ(num_orders, std::int32_t, in);
  RFEED_READ(px, std::int64_t, in);
  RFEED_READ(size, std::int64_t, in);

  return SnapshotEntry{
      .px          = Mantissa{*px},
      .size        = *size,
      .num_orders  = *num_orders,
      .price_level = *price_level,
      .type        = to_entry_type(*type),
  };
}

std::expected<InstrumentDefinition, DecodeError> AltCodec::instrument_definition(
    ByteReader& in) noexcept {
  RFEED_READ(security, std::int32_t, in);
  RFEED_READ(min_price_increment, std::int64_t, in);
  RFEED_READ(tot_num_reports, std::uint32_t, in);
  RFEED_READ(market_depth, std::uint8_t, in);
  RFEED_READ(sub_tick_divisor, std::uint8_t, in);

  const auto symbol = in.read_bytes(kMaxSymbolLen);
  if (!symbol) return std::unexpected{symbol.error()};

  InstrumentDefinition def{};
  def.security = *security;
  for (std::size_t i = 0; i < kMaxSymbolLen; ++i) {
    def.symbol[i] = static_cast<char>(std::to_integer<std::uint8_t>((*symbol)[i]));
  }
  def.min_price_increment = Mantissa{*min_price_increment};
  def.tot_num_reports     = *tot_num_reports;
  def.market_depth        = *market_depth;
  def.sub_tick_divisor    = (*sub_tick_divisor == 0) ? std::uint8_t{1} : *sub_tick_divisor;
  return def;
}

std::expected<SecurityStatusMsg, DecodeError> AltCodec::security_status(ByteReader& in) noexcept {
  RFEED_READ(transact_time, std::uint64_t, in);
  RFEED_READ(security, std::int32_t, in);
  RFEED_READ(status, std::uint8_t, in);

  return SecurityStatusMsg{
      .security      = *security,
      .transact_time = static_cast<Timestamp>(*transact_time),
      .status        = to_trading_status(*status),
  };
}

std::expected<ChannelResetMsg, DecodeError> AltCodec::channel_reset(ByteReader& in) noexcept {
  RFEED_READ(kind, std::uint8_t, in);
  RFEED_SKIP(in, 3);
  RFEED_READ(transact_time, std::uint64_t, in);

  return ChannelResetMsg{
      .transact_time = static_cast<Timestamp>(*transact_time),
      .kind          = (*kind == 1) ? ResetKind::PeriodStart : ResetKind::Intraday,
  };
}

#undef RFEED_READ
#undef RFEED_SKIP

}  // namespace rfeed::venue_alt
