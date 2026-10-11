#pragma once

// Synthetic MDP 3.0 feed generator.
//
// This is the mitigation for the project's largest schedule risk: with no
// capture from the venue, every phase after framing would otherwise be written
// blind. It is also the differential test — the reference book model below is
// keyed by PRICE (std::map), while MbpBook is addressed by LEVEL INDEX. Two
// independent representations that must agree, so a level-manipulation bug in
// one cannot be mirrored by the same bug in the other.
//
// Snapshots are produced from the reference model, which makes them a genuine
// oracle rather than a replay of the handler's own output.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ref_encoder.hpp"
#include "ref_venue.hpp"
#include "rfeed/channel_config.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/pcap.hpp"

namespace rfeed::test {

constexpr std::uint32_t kIncrA = ipv4(224, 0, 1, 1);
constexpr std::uint32_t kIncrB = ipv4(224, 0, 1, 2);
constexpr std::uint32_t kSnapA = ipv4(224, 0, 1, 3);
constexpr std::uint32_t kSnapB = ipv4(224, 0, 1, 4);
constexpr std::uint32_t kDefA  = ipv4(224, 0, 1, 5);
constexpr std::uint32_t kDefB  = ipv4(224, 0, 1, 6);

constexpr std::uint16_t kPortIncrA = 14310;
constexpr std::uint16_t kPortIncrB = 15310;
constexpr std::uint16_t kPortSnapA = 14311;
constexpr std::uint16_t kPortSnapB = 15311;
constexpr std::uint16_t kPortDefA  = 14312;
constexpr std::uint16_t kPortDefB  = 15312;

inline ChannelConfig synthetic_config() {
  ChannelConfig cfg;
  cfg.id          = 310;
  cfg.name        = "ref-310";
  cfg.connections = {
      {ChannelKind::Incremental, LineId::A, Endpoint{kIncrA, kPortIncrA, 0}},
      {ChannelKind::Incremental, LineId::B, Endpoint{kIncrB, kPortIncrB, 0}},
      {ChannelKind::Snapshot, LineId::A, Endpoint{kSnapA, kPortSnapA, 0}},
      {ChannelKind::Snapshot, LineId::B, Endpoint{kSnapB, kPortSnapB, 0}},
      {ChannelKind::InstrumentDefinition, LineId::A, Endpoint{kDefA, kPortDefA, 0}},
      {ChannelKind::InstrumentDefinition, LineId::B, Endpoint{kDefB, kPortDefB, 0}},
  };
  return cfg;
}

// Price-keyed reference book. Deliberately a different data structure from the
// one under test.
struct RefBook {
  std::map<std::int64_t, std::int64_t, std::greater<>> bids;  // best first
  std::map<std::int64_t, std::int64_t>                 asks;  // best first

  [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> top(bool bid,
                                                                       std::size_t depth) const {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    if (bid) {
      for (const auto& [px, qty] : bids) {
        if (out.size() == depth) break;
        out.emplace_back(px, qty);
      }
    } else {
      for (const auto& [px, qty] : asks) {
        if (out.size() == depth) break;
        out.emplace_back(px, qty);
      }
    }
    return out;
  }
};

struct FeedParams {
  std::uint32_t              instruments{3};
  SecurityId                 first_security{100};
  std::uint32_t              steps{400};
  std::uint8_t               depth{10};
  std::uint32_t              snapshot_period{50};  // incremental packets between snapshot cycles
  std::uint32_t              definition_cycles{2};
  std::int64_t               min_price_increment{25'000'000};
  std::uint64_t              seed{0x2A2A2A2A};

  // Wall-clock spacing between packets. This has to be comfortably larger than
  // the venue's gap deadline or a hole never matures into a gap and the recovery
  // path is silently never tested.
  Timestamp                  packet_interval_ns{1'000'000};

  // Incremental steps emitted between consecutive instruments of a snapshot
  // cycle. Non-zero on purpose: it is what makes some buffered incrementals
  // POSTDATE their own instrument's snapshot, which is the only thing that
  // exercises the RecoveryReplay filter. With an atomic cycle, every buffered
  // packet is older than every snapshot and the replay applies nothing.
  std::uint32_t              snapshot_interleave{2};
  // Return true to drop this incremental packet on this line.
  std::function<bool(SeqNum, LineId)> drop{};
};

// Templated on the venue's encoder so the identical market data, the identical
// loss patterns and the identical convergence gate can be replayed against any
// venue. A core that had quietly specialised on one venue's wire format would
// fail here and nowhere else.
template <class EncoderT>
class SyntheticFeedT {
 public:
  explicit SyntheticFeedT(FeedParams params) : params_{std::move(params)} {
    for (std::uint32_t i = 0; i < params_.instruments; ++i) {
      const auto id = params_.first_security + static_cast<SecurityId>(i);
      securities_.push_back(id);
      models_[id] = RefBook{};
    }
  }

  [[nodiscard]] std::expected<void, PcapError> write(const std::string& path) {
    auto writer = PcapWriter::create(path);
    if (!writer) return std::unexpected{writer.error()};

    emit_definitions(*writer);
    emit_stream(*writer);

    return writer->close();
  }

  // The expected end state, from the independent model.
  [[nodiscard]] const std::map<SecurityId, RefBook>& models() const noexcept { return models_; }
  [[nodiscard]] const std::vector<SecurityId>& securities() const noexcept { return securities_; }
  [[nodiscard]] SeqNum last_incremental_seq() const noexcept { return incr_seq_ - 1; }

 private:
  [[nodiscard]] std::uint64_t rand_next() noexcept {
    seed_ = seed_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed_ >> 33;
  }

  [[nodiscard]] std::uint64_t rand_below(std::uint64_t n) noexcept { return rand_next() % n; }

  void write_both_lines(PcapWriter& writer, std::span<const std::byte> payload, SeqNum seq,
                        std::uint32_t group_a, std::uint16_t port_a, std::uint32_t group_b,
                        std::uint16_t port_b, bool droppable) {
    const auto drop_a = droppable && params_.drop && params_.drop(seq, LineId::A);
    const auto drop_b = droppable && params_.drop && params_.drop(seq, LineId::B);

    if (!drop_a) {
      (void)writer.write_udp(payload, ts_, ipv4(192, 0, 2, 1), 1000, group_a, port_a);
    }
    constexpr Timestamp kLineSkew = 1'000;  // B trails A by a microsecond
    ts_ += kLineSkew;
    if (!drop_b) {
      (void)writer.write_udp(payload, ts_, ipv4(192, 0, 2, 2), 1000, group_b, port_b);
    }
    ts_ += params_.packet_interval_ns > kLineSkew ? params_.packet_interval_ns - kLineSkew
                                                  : kLineSkew;
  }

  void emit_definitions(PcapWriter& writer) {
    for (std::uint32_t cycle = 0; cycle < params_.definition_cycles; ++cycle) {
      for (const auto id : securities_) {
        InstrumentDefinition def{};
        def.security = id;
        const auto name = "SYN" + std::to_string(id);
        for (std::size_t i = 0; i < name.size() && i < kMaxSymbolLen; ++i) def.symbol[i] = name[i];
        def.min_price_increment = Mantissa{params_.min_price_increment};
        def.tot_num_reports     = static_cast<std::uint32_t>(securities_.size());
        def.market_depth        = params_.depth;
        def.sub_tick_divisor    = 1;

        EncoderT enc{def_seq_++, ts_};
        enc.instrument_definition(def);
        write_both_lines(writer, enc.view(), 0, kDefA, kPortDefA, kDefB, kPortDefB, false);
      }
    }
  }

  void emit_stream(PcapWriter& writer) {
    for (std::uint32_t step = 0; step < params_.steps; ++step) {
      emit_incremental_step(writer);

      if (params_.snapshot_period > 0 && step % params_.snapshot_period == params_.snapshot_period - 1) {
        emit_snapshot_cycle(writer);
      }
    }
    // Trailing cycles so a handler that gapped near the end still has a join
    // point, and so the last recovery has somewhere to converge.
    emit_snapshot_cycle(writer);
    emit_snapshot_cycle(writer);
  }

  void emit_incremental_step(PcapWriter& writer) {
    const auto id      = securities_[rand_below(securities_.size())];
    const auto entries = mutate(id);
    if (entries.empty()) return;

    EncoderT enc{incr_seq_, ts_};
    enc.incremental(IncrementalHeader{.transact_time = ts_, .end_of_event = true}, entries);
    write_both_lines(writer, enc.view(), incr_seq_, kIncrA, kPortIncrA, kIncrB, kPortIncrB, true);
    ++incr_seq_;
  }

  void emit_snapshot_cycle(PcapWriter& writer) {
    bool first = true;
    for (const auto id : securities_) {
      // Incrementals keep flowing while the loop is being published — that is
      // what a real snapshot channel looks like, and it is what puts buffered
      // packets on both sides of each instrument's join point.
      if (!first) {
        for (std::uint32_t i = 0; i < params_.snapshot_interleave; ++i) {
          emit_incremental_step(writer);
        }
      }
      first = false;

      const auto& model = models_[id];

      std::vector<SnapshotEntry> entries;
      const auto                 bids = model.top(true, params_.depth);
      const auto                 asks = model.top(false, params_.depth);
      for (std::size_t i = 0; i < bids.size(); ++i) {
        entries.push_back(SnapshotEntry{Mantissa{bids[i].first}, bids[i].second, 1,
                                        static_cast<std::uint8_t>(i + 1), EntryType::Bid});
      }
      for (std::size_t i = 0; i < asks.size(); ++i) {
        entries.push_back(SnapshotEntry{Mantissa{asks[i].first}, asks[i].second, 1,
                                        static_cast<std::uint8_t>(i + 1), EntryType::Offer});
      }

      const SnapshotHeader header{
          .last_msg_seq_num_processed = incr_seq_ - 1,
          .tot_num_reports            = static_cast<std::uint32_t>(securities_.size()),
          .security                   = id,
          .rpt_seq                    = rpt_seq_[id],
          .transact_time              = ts_,
          .status                     = TradingStatus::Open,
      };

      EncoderT enc{snap_seq_++, ts_};
      enc.snapshot(header, entries);
      write_both_lines(writer, enc.view(), 0, kSnapA, kPortSnapA, kSnapB, kPortSnapB, false);
    }
  }

  // One random mutation, converted into the level-addressed entries the venue
  // would publish. Rank computation is what turns a price-keyed change into an
  // MDP price level, and is the part the handler has to invert correctly.
  std::vector<MdEntry> mutate(SecurityId id) {
    auto&      model  = models_[id];
    const bool is_bid = rand_below(2) == 0;

    const auto tick      = params_.min_price_increment;
    const auto base      = is_bid ? std::int64_t{4'000} : std::int64_t{4'010};
    const auto px        = (base + static_cast<std::int64_t>(rand_below(12))) * tick;
    const auto entry_type = is_bid ? EntryType::Bid : EntryType::Offer;
    const auto depth     = static_cast<std::size_t>(params_.depth);

    const auto before      = model.top(is_bid, depth);
    const bool existed     = is_bid ? model.bids.contains(px) : model.asks.contains(px);
    const auto action_roll = rand_below(10);

    std::vector<MdEntry> out;

    if (existed && action_roll < 3) {
      // Remove.
      const auto rank = rank_of(before, px);
      if (is_bid) model.bids.erase(px); else model.asks.erase(px);
      if (rank) {
        out.push_back(entry(id, Mantissa{px}, 0, static_cast<std::uint8_t>(*rank + 1),
                            UpdateAction::Delete, entry_type));
        // Deleting a visible level pulls a previously-hidden one into view.
        const auto after = model.top(is_bid, depth);
        if (after.size() == depth && before.size() == depth) {
          const auto last = static_cast<std::size_t>(depth) - 1;
          out.push_back(entry(id, Mantissa{after[last].first}, after[last].second,
                              static_cast<std::uint8_t>(depth), UpdateAction::New, entry_type));
        }
      }
    } else if (existed) {
      // Change quantity in place.
      const auto qty  = static_cast<std::int64_t>(1 + rand_below(500));
      const auto rank = rank_of(before, px);
      if (is_bid) model.bids[px] = qty; else model.asks[px] = qty;
      if (rank) {
        out.push_back(entry(id, Mantissa{px}, qty, static_cast<std::uint8_t>(*rank + 1),
                            UpdateAction::Change, entry_type));
      }
    } else {
      // Insert.
      const auto qty = static_cast<std::int64_t>(1 + rand_below(500));
      if (is_bid) model.bids[px] = qty; else model.asks[px] = qty;
      const auto after = model.top(is_bid, depth);
      const auto rank  = rank_of(after, px);
      if (rank) {
        out.push_back(entry(id, Mantissa{px}, qty, static_cast<std::uint8_t>(*rank + 1),
                            UpdateAction::New, entry_type));
      }
    }

    for (auto& e : out) e.rpt_seq = ++rpt_seq_[id];
    return out;
  }

  [[nodiscard]] static std::optional<std::size_t> rank_of(
      const std::vector<std::pair<std::int64_t, std::int64_t>>& levels, std::int64_t px) {
    for (std::size_t i = 0; i < levels.size(); ++i) {
      if (levels[i].first == px) return i;
    }
    return std::nullopt;
  }

  [[nodiscard]] static MdEntry entry(SecurityId id, Mantissa px, std::int64_t qty,
                                     std::uint8_t level, UpdateAction action, EntryType type) {
    MdEntry e{};
    e.px          = px;
    e.size        = qty;
    e.security    = id;
    e.num_orders  = 1;
    e.price_level = level;
    e.action      = action;
    e.type        = type;
    return e;
  }

  FeedParams                        params_;
  std::vector<SecurityId>           securities_{};
  std::map<SecurityId, RefBook>     models_{};
  std::map<SecurityId, RptSeq>      rpt_seq_{};
  std::uint64_t                     seed_{params_.seed};
  Timestamp                         ts_{1'700'000'000'000'000'000};
  SeqNum                            incr_seq_{1};
  SeqNum                            snap_seq_{1};
  SeqNum                            def_seq_{1};
};

using SyntheticFeed = SyntheticFeedT<venue_ref::Encoder>;

}  // namespace rfeed::test
