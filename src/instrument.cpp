#include "rfeed/instrument.hpp"

#include <bit>
#include <cstdint>

namespace rfeed {
namespace {

[[nodiscard]] std::size_t round_up_pow2(std::size_t n) noexcept {
  if (n < 2) return 2;
  return std::bit_ceil(n);
}

// SecurityIDs are dense and sequential on most venues, so a cheap mix is enough
// to avoid clustering without paying for a real hash.
[[nodiscard]] std::size_t mix(SecurityId id) noexcept {
  auto x = static_cast<std::uint64_t>(static_cast<std::uint32_t>(id));
  x *= 0x9E3779B97F4A7C15ULL;
  x ^= x >> 29;
  return static_cast<std::size_t>(x);
}

}  // namespace

InstrumentStore::InstrumentStore(std::size_t capacity) {
  // Keep the load factor at or below 1/2: linear probing degrades sharply past
  // that, and the whole point of this table is a bounded probe on the hot path.
  const auto slots = round_up_pow2(capacity * 2);
  slots_.assign(slots, Slot{});
  mask_ = slots - 1;
}

std::size_t InstrumentStore::index_of(SecurityId id) const noexcept {
  std::size_t idx = mix(id) & mask_;
  for (std::size_t probe = 0; probe <= mask_; ++probe) {
    const auto& slot = slots_[idx];
    if (!slot.occupied || slot.def.security == id) return idx;
    idx = (idx + 1) & mask_;
  }
  return idx;
}

bool InstrumentStore::upsert(const InstrumentDefinition& def) noexcept {
  const auto idx  = index_of(def.security);
  auto&      slot = slots_[idx];

  if (!slot.occupied) {
    if (count_ * 2 >= slots_.size()) return false;  // table full; never rehash mid-session
    slot.occupied = true;
    ++count_;
  }
  slot.def = def;
  return true;
}

const InstrumentDefinition* InstrumentStore::find(SecurityId id) const noexcept {
  const auto& slot = slots_[index_of(id)];
  if (!slot.occupied || slot.def.security != id) return nullptr;
  return &slot.def;
}

void InstrumentStore::clear() noexcept {
  for (auto& slot : slots_) slot.occupied = false;
  count_ = 0;
}

void InstrumentStore::for_each(const std::function<void(const InstrumentDefinition&)>& fn) const {
  for (const auto& slot : slots_) {
    if (slot.occupied) fn(slot.def);
  }
}

std::expected<Price, ConversionError> InstrumentStore::to_ticks(
    const InstrumentDefinition& def, Mantissa px) noexcept {
  const auto increment = def.min_price_increment.raw;
  if (increment <= 0) return std::unexpected{ConversionError::ZeroIncrement};

  const auto divisor = static_cast<std::int64_t>(def.sub_tick_divisor == 0 ? 1
                                                                           : def.sub_tick_divisor);
  if (increment % divisor != 0) return std::unexpected{ConversionError::BadSubTick};

  const auto step = increment / divisor;
  if (step == 0) return std::unexpected{ConversionError::ZeroIncrement};

  // Truncating here is the bug this whole function exists to prevent.
  if (px.raw % step != 0) return std::unexpected{ConversionError::SubTickPrice};

  return Price{px.raw / step};
}

std::expected<Mantissa, ConversionError> InstrumentStore::to_mantissa(
    const InstrumentDefinition& def, Price ticks) noexcept {
  const auto increment = def.min_price_increment.raw;
  if (increment <= 0) return std::unexpected{ConversionError::ZeroIncrement};

  const auto divisor = static_cast<std::int64_t>(def.sub_tick_divisor == 0 ? 1
                                                                           : def.sub_tick_divisor);
  if (increment % divisor != 0) return std::unexpected{ConversionError::BadSubTick};

  return Mantissa{ticks.ticks * (increment / divisor)};
}

std::expected<Price, ConversionError> InstrumentStore::to_ticks(SecurityId id,
                                                                Mantissa px) const noexcept {
  const auto* def = find(id);
  if (def == nullptr) return std::unexpected{ConversionError::NoInstrument};
  return to_ticks(*def, px);
}

}  // namespace rfeed
