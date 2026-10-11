#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>
#include <vector>

#include "rfeed/messages.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

enum class ConversionError : std::uint8_t {
  NoInstrument,   // no definition seen for this SecurityID yet
  ZeroIncrement,  // definition carries a zero/negative tick size
  BadSubTick,     // sub_tick_divisor does not divide min_price_increment exactly
  SubTickPrice,   // the price is not an exact multiple of the finest increment
};

[[nodiscard]] constexpr std::string_view to_string(ConversionError e) noexcept {
  switch (e) {
    case ConversionError::NoInstrument:  return "NoInstrument";
    case ConversionError::ZeroIncrement: return "ZeroIncrement";
    case ConversionError::BadSubTick:    return "BadSubTick";
    case ConversionError::SubTickPrice:  return "SubTickPrice";
  }
  return "Unknown";
}

// Open-addressed SecurityID → definition map, sized once at construction and
// never resized. One allocation at startup, none on the hot path.
//
// Keyed on SecurityId alone because a store belongs to exactly one channel;
// SecurityKey's channel component is applied at the layer above.
class InstrumentStore {
 public:
  explicit InstrumentStore(std::size_t capacity = 4096);

  // Definitions arrive on the definition-replay loop at startup and again on the
  // incremental feed intraday — new listings, expiry changes. A SecurityID
  // appearing mid-session is normal, not an error.
  bool upsert(const InstrumentDefinition& def) noexcept;

  [[nodiscard]] const InstrumentDefinition* find(SecurityId id) const noexcept;
  [[nodiscard]] bool contains(SecurityId id) const noexcept { return find(id) != nullptr; }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

  void clear() noexcept;

  void for_each(const std::function<void(const InstrumentDefinition&)>& fn) const;

  // Mantissa → integer ticks. Fallible on purpose.
  //
  // `mantissa / min_price_increment` is NOT always exact: spreads and implied
  // prices routinely land on sub-tick increments, and integer division would
  // silently truncate them into the wrong price. That failure looks correct for
  // weeks and then isn't, so the error is surfaced rather than swallowed.
  //
  // A "tick" here is the instrument's FINEST addressable increment —
  // min_price_increment / sub_tick_divisor — so a product quoting in half-ticks
  // has twice as many ticks per point as its outright.
  [[nodiscard]] std::expected<Price, ConversionError> to_ticks(SecurityId id,
                                                               Mantissa px) const noexcept;

  // Same conversion against a definition the caller already holds.
  [[nodiscard]] static std::expected<Price, ConversionError> to_ticks(
      const InstrumentDefinition& def, Mantissa px) noexcept;

  // Inverse, for display and for round-trip tests.
  [[nodiscard]] static std::expected<Mantissa, ConversionError> to_mantissa(
      const InstrumentDefinition& def, Price ticks) noexcept;

 private:
  struct Slot {
    InstrumentDefinition def{};
    bool                 occupied{false};
  };

  [[nodiscard]] std::size_t index_of(SecurityId id) const noexcept;

  std::vector<Slot> slots_;
  std::size_t       count_{0};
  std::size_t       mask_{0};
};

}  // namespace rfeed
