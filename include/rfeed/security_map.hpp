#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "rfeed/types.hpp"

namespace rfeed {

// Open-addressed SecurityID → T, allocated once and never resized.
//
// Sized at construction because rehashing mid-session would allocate on the hot
// path and invalidate every borrowed reference the sink might be holding. A
// full table is reported, not grown.
template <class T>
class SecurityMap {
 public:
  explicit SecurityMap(std::size_t capacity = 4096) {
    const auto slots = std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity * 2);
    slots_.assign(slots, Slot{});
    mask_ = slots - 1;
  }

  [[nodiscard]] T* find(SecurityId id) noexcept {
    auto& slot = slots_[index_of(id)];
    if (!slot.occupied || slot.id != id) return nullptr;
    return &slot.value;
  }

  [[nodiscard]] const T* find(SecurityId id) const noexcept {
    const auto& slot = slots_[index_of(id)];
    if (!slot.occupied || slot.id != id) return nullptr;
    return &slot.value;
  }

  // Returns nullptr only when the table is full — a configuration error, and
  // one the caller must surface rather than silently drop an instrument.
  [[nodiscard]] T* emplace(SecurityId id) noexcept {
    auto& slot = slots_[index_of(id)];
    if (slot.occupied && slot.id == id) return &slot.value;
    if (count_ * 2 >= slots_.size()) return nullptr;
    slot.occupied = true;
    slot.id       = id;
    slot.value    = T{};
    ++count_;
    return &slot.value;
  }

  void clear() noexcept {
    for (auto& slot : slots_) slot.occupied = false;
    count_ = 0;
  }

  // Clears values but keeps the key set, for a reset that empties books without
  // forgetting which instruments exist.
  void reset_values() noexcept {
    for (auto& slot : slots_) {
      if (slot.occupied) slot.value = T{};
    }
  }

  void for_each(const std::function<void(SecurityId, T&)>& fn) {
    for (auto& slot : slots_) {
      if (slot.occupied) fn(slot.id, slot.value);
    }
  }

  void for_each(const std::function<void(SecurityId, const T&)>& fn) const {
    for (const auto& slot : slots_) {
      if (slot.occupied) fn(slot.id, slot.value);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

 private:
  struct Slot {
    T          value{};
    SecurityId id{0};
    bool       occupied{false};
  };

  [[nodiscard]] std::size_t index_of(SecurityId id) const noexcept {
    auto x = static_cast<std::uint64_t>(static_cast<std::uint32_t>(id));
    x *= 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    std::size_t idx = static_cast<std::size_t>(x) & mask_;
    for (std::size_t probe = 0; probe <= mask_; ++probe) {
      const auto& slot = slots_[idx];
      if (!slot.occupied || slot.id == id) return idx;
      idx = (idx + 1) & mask_;
    }
    return idx;
  }

  std::vector<Slot> slots_;
  std::size_t       count_{0};
  std::size_t       mask_{0};
};

}  // namespace rfeed
