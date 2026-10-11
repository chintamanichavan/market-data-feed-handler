#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rfeed/transport.hpp"
#include "rfeed/types.hpp"

namespace rfeed {

// Fixed-capacity FIFO of whole datagrams, held while the snapshot loop is being
// joined and replayed on top of the installed snapshots.
//
// Allocated once at construction. Overflow drops the OLDEST entry and counts it:
// the newest packets are the ones we cannot reconstruct from a snapshot, and a
// silently truncated buffer would read as a clean recovery when it was not.
class PacketBuffer {
 public:
  struct Entry {
    SeqNum                     seq{};
    Timestamp                  rx_ts{};
    std::span<const std::byte> bytes{};
  };

  explicit PacketBuffer(std::size_t capacity = 1024) : slots_(capacity == 0 ? 1 : capacity) {}

  void push(SeqNum seq, std::span<const std::byte> bytes, Timestamp rx_ts) noexcept {
    if (count_ == slots_.size()) {
      head_ = (head_ + 1) % slots_.size();
      --count_;
      ++dropped_;
    }
    const auto idx = (head_ + count_) % slots_.size();
    auto&      s   = slots_[idx];
    s.len          = std::min(bytes.size(), kMaxDatagramBytes);
    std::copy_n(bytes.begin(), s.len, s.storage.begin());
    s.seq   = seq;
    s.rx_ts = rx_ts;
    ++count_;
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (std::size_t i = 0; i < count_; ++i) {
      const auto& s = slots_[(head_ + i) % slots_.size()];
      fn(Entry{s.seq, s.rx_ts, std::span<const std::byte>{s.storage.data(), s.len}});
    }
  }

  void clear() noexcept {
    head_  = 0;
    count_ = 0;
  }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

 private:
  struct Slot {
    std::array<std::byte, kMaxDatagramBytes> storage{};
    std::size_t                              len{0};
    SeqNum                                   seq{0};
    Timestamp                                rx_ts{0};
  };

  std::vector<Slot> slots_;
  std::size_t       head_{0};
  std::size_t       count_{0};
  std::uint64_t     dropped_{0};
};

}  // namespace rfeed
