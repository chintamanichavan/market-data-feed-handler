#pragma once

#include <cstdint>
#include <optional>

#include "rfeed/line_arbiter.hpp"
#include "rfeed/messages.hpp"
#include "rfeed/normalized.hpp"
#include "rfeed/types.hpp"
#include "rfeed/venue.hpp"

namespace rfeed {

struct RecoveryConfig {
  // A definition cycle that never completes means we are pointed at the wrong
  // channel or the venue is not publishing. Fail loudly rather than hang.
  Timestamp definition_cycle_timeout_ns{60'000'000'000};

  // Snapshot cycles are slow — seconds to tens of seconds on a large channel.
  Timestamp snapshot_cycle_timeout_ns{120'000'000'000};

  // Give up and declare Failed after this many consecutive snapshot cycles that
  // never completed.
  std::uint32_t max_snapshot_attempts{3};

  bool replay_available{false};

  // Some venues publish definitions only on the incremental feed.
  bool has_definition_channel{true};
};

// Tracks progress through one pass of the continuously-looping snapshot channel.
//
// The loop is joined mid-cycle, so completion cannot be detected by waiting for
// a "last" marker. Three independent signals, because the first two are
// underspecified across venues and the third must never be the only one:
//   1. distinct instruments seen reaches TotNumReports
//   2. the first instrument observed comes round again
//   3. wall-clock backstop
class SnapshotCycle {
 public:
  void begin(Timestamp now) noexcept {
    started_ = now;
    first_.reset();
    seen_          = 0;
    tot_reports_   = 0;
    wrapped_       = false;
    active_        = true;
  }

  // Returns true when this report completes the cycle.
  bool observe(const SnapshotHeader& header) noexcept {
    if (!active_) return false;
    tot_reports_ = header.tot_num_reports;

    if (!first_) {
      first_ = header.security;
    } else if (*first_ == header.security) {
      wrapped_ = true;
    }
    ++seen_;
    return complete_now();
  }

  [[nodiscard]] bool timed_out(Timestamp now, Timestamp timeout_ns) const noexcept {
    return active_ && (now - started_) >= timeout_ns;
  }

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] std::uint32_t reports_seen() const noexcept { return seen_; }
  [[nodiscard]] std::uint32_t tot_reports() const noexcept { return tot_reports_; }
  [[nodiscard]] bool wrapped() const noexcept { return wrapped_; }

  void end() noexcept { active_ = false; }

 private:
  [[nodiscard]] bool complete_now() const noexcept {
    if (wrapped_) return true;
    return tot_reports_ > 0 && seen_ >= tot_reports_;
  }

  Timestamp                 started_{0};
  std::optional<SecurityId> first_{};
  std::uint32_t             seen_{0};
  std::uint32_t             tot_reports_{0};
  bool                      wrapped_{false};
  bool                      active_{false};
};

// Tracks a pass over the instrument-definition replay loop, which uses the same
// TotNumReports convention.
class DefinitionCycle {
 public:
  void begin(Timestamp now) noexcept {
    started_     = now;
    seen_        = 0;
    tot_reports_ = 0;
    first_.reset();
    wrapped_ = false;
    active_  = true;
  }

  bool observe(const InstrumentDefinition& def) noexcept {
    if (!active_) return false;
    tot_reports_ = def.tot_num_reports;
    if (!first_) {
      first_ = def.security;
    } else if (*first_ == def.security) {
      wrapped_ = true;
    }
    ++seen_;
    return wrapped_ || (tot_reports_ > 0 && seen_ >= tot_reports_);
  }

  [[nodiscard]] bool timed_out(Timestamp now, Timestamp timeout_ns) const noexcept {
    return active_ && (now - started_) >= timeout_ns;
  }

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] std::uint32_t reports_seen() const noexcept { return seen_; }

  void end() noexcept { active_ = false; }

 private:
  Timestamp                 started_{0};
  std::optional<SecurityId> first_{};
  std::uint32_t             seen_{0};
  std::uint32_t             tot_reports_{0};
  bool                      wrapped_{false};
  bool                      active_{false};
};

// What the caller should do as a result of the last transition.
struct RecoveryDirective {
  bool buffer_incrementals{false};  // hold packets instead of applying them
  bool publish{true};               // emit normalized events
  bool request_replay{false};       // issue a TCP replay for pending_gap
  bool restart_snapshot{false};     // (re)join the snapshot loop
  bool clear_books{false};
};

// The recovery state machine. Pure: no I/O, no clocks of its own, no ownership
// of books. Every input is an explicit call, which is what makes the gap-shape
// test matrix in recovery_test.cpp possible at all.
template <VenuePolicy V>
class RecoveryFsm {
 public:
  explicit RecoveryFsm(RecoveryConfig config = {}) : config_{config} {}

  [[nodiscard]] FeedState state() const noexcept { return state_; }

  // True when books are being rebuilt by natural refresh while real recovery
  // runs underneath. Consumers see BookState::Degraded and decide for
  // themselves; we never pretend the book is authoritative.
  [[nodiscard]] bool degraded() const noexcept { return degraded_; }

  [[nodiscard]] const RecoveryConfig& config() const noexcept { return config_; }
  [[nodiscard]] SnapshotCycle& snapshot_cycle() noexcept { return snapshot_cycle_; }
  [[nodiscard]] DefinitionCycle& definition_cycle() noexcept { return definition_cycle_; }
  [[nodiscard]] std::uint32_t snapshot_attempts() const noexcept { return snapshot_attempts_; }
  [[nodiscard]] std::uint64_t transitions() const noexcept { return transitions_; }

  RecoveryDirective start(Timestamp now) noexcept {
    if (config_.has_definition_channel) {
      definition_cycle_.begin(now);
      return enter(FeedState::AwaitingDefinitions, now);
    }
    // Definitions ride the incremental feed on this venue; go straight to
    // snapshot sync and pick them up as they arrive.
    snapshot_cycle_.begin(now);
    return enter(FeedState::SnapshotSync, now);
  }

  RecoveryDirective on_definitions_complete(Timestamp now) noexcept {
    if (state_ != FeedState::AwaitingDefinitions) return current_directive();
    definition_cycle_.end();
    snapshot_cycle_.begin(now);
    return enter(FeedState::SnapshotSync, now);
  }

  // A matured gap — never a bare hole. Chooses replay when the range is bounded
  // and within the venue's per-request limit, snapshot otherwise.
  RecoveryDirective on_gap(const GapRange& gap, Timestamp now) noexcept {
    if (state_ == FeedState::Failed) return current_directive();

    // Natural refresh runs in parallel: clear the affected books, keep applying
    // incrementals, and mark everything Degraded. This yields a usable book in
    // milliseconds without ever claiming it is authoritative.
    degraded_ = true;

    const bool bounded    = gap.count() > 0;
    const bool within_cap = bounded && gap.count() <= V::kReplayLimits.max_packets_per_request;

    if (config_.replay_available && within_cap) {
      auto directive           = enter(FeedState::ReplaySync, now);
      directive.request_replay = true;
      directive.clear_books    = true;
      return directive;
    }

    snapshot_cycle_.begin(now);
    auto directive             = enter(FeedState::SnapshotSync, now);
    directive.restart_snapshot = true;
    directive.clear_books      = true;
    return directive;
  }

  RecoveryDirective on_snapshot_cycle_complete(Timestamp now) noexcept {
    if (state_ != FeedState::SnapshotSync) return current_directive();
    snapshot_cycle_.end();
    snapshot_attempts_ = 0;
    degraded_          = false;
    return enter(FeedState::Synchronized, now);
  }

  RecoveryDirective on_replay_complete(Timestamp now) noexcept {
    if (state_ != FeedState::ReplaySync) return current_directive();
    degraded_ = false;
    return enter(FeedState::Synchronized, now);
  }

  // Replay refused, timed out, or hit a rate limit. Snapshot is always the
  // fallback — a single replay failure is never terminal.
  RecoveryDirective on_replay_failed(Timestamp now) noexcept {
    if (state_ != FeedState::ReplaySync) return current_directive();
    snapshot_cycle_.begin(now);
    auto directive             = enter(FeedState::SnapshotSync, now);
    directive.restart_snapshot = true;
    return directive;
  }

  // The venue restarted the channel. Everything goes back to the beginning;
  // what exactly gets cleared is the venue's ResetScope, applied by the caller.
  RecoveryDirective on_channel_reset(Timestamp now) noexcept {
    degraded_          = false;
    snapshot_attempts_ = 0;
    snapshot_cycle_.end();
    definition_cycle_.end();
    auto directive        = start(now);
    directive.clear_books = true;
    return directive;
  }

  // Wall-clock backstop. Both cycle detectors are underspecified across venues,
  // so neither is allowed to be the only way out.
  RecoveryDirective on_tick(Timestamp now) noexcept {
    switch (state_) {
      case FeedState::AwaitingDefinitions:
        if (definition_cycle_.timed_out(now, config_.definition_cycle_timeout_ns)) {
          definition_cycle_.end();
          return enter(FeedState::Failed, now);
        }
        break;

      case FeedState::SnapshotSync:
        if (snapshot_cycle_.timed_out(now, config_.snapshot_cycle_timeout_ns)) {
          ++snapshot_attempts_;
          if (snapshot_attempts_ >= config_.max_snapshot_attempts) {
            snapshot_cycle_.end();
            return enter(FeedState::Failed, now);
          }
          snapshot_cycle_.begin(now);
          auto directive             = current_directive();
          directive.restart_snapshot = true;
          return directive;
        }
        break;

      case FeedState::Init:
      case FeedState::ReplaySync:
      case FeedState::Synchronized:
      case FeedState::Failed:
        break;
    }
    return current_directive();
  }

  // The reorder window was blown while already recovering: restart the join
  // rather than compounding an unknown gap on top of an incomplete book.
  RecoveryDirective on_window_blown(Timestamp now) noexcept {
    if (state_ == FeedState::Failed) return current_directive();
    degraded_ = true;
    snapshot_cycle_.begin(now);
    auto directive             = enter(FeedState::SnapshotSync, now);
    directive.restart_snapshot = true;
    directive.clear_books      = true;
    return directive;
  }

  [[nodiscard]] RecoveryDirective current_directive() const noexcept {
    return RecoveryDirective{
        .buffer_incrementals = state_ == FeedState::SnapshotSync || state_ == FeedState::ReplaySync,
        .publish             = state_ != FeedState::AwaitingDefinitions && state_ != FeedState::Init,
        .request_replay      = false,
        .restart_snapshot    = false,
        .clear_books         = false,
    };
  }

  [[nodiscard]] std::optional<FeedStateChange> take_transition() noexcept {
    auto out = last_transition_;
    last_transition_.reset();
    return out;
  }

 private:
  RecoveryDirective enter(FeedState next, Timestamp now) noexcept {
    if (next != state_) {
      ++transitions_;
      last_transition_ = FeedStateChange{.from = state_, .to = next, .rx_ts = now};
      state_           = next;
    }
    return current_directive();
  }

  RecoveryConfig                 config_{};
  SnapshotCycle                  snapshot_cycle_{};
  DefinitionCycle                definition_cycle_{};
  std::optional<FeedStateChange> last_transition_{};
  FeedState                      state_{FeedState::Init};
  std::uint32_t                  snapshot_attempts_{0};
  std::uint64_t                  transitions_{0};
  bool                           degraded_{false};
};

}  // namespace rfeed
