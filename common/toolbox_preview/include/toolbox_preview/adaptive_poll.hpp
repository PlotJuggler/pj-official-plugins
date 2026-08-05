// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace toolbox_preview {

/// Cadence policy for a per-tick "should I do the expensive read now?" decision, kept
/// separate from the read mechanism so the rule lives in one named, testable place.
///
/// Behaviour: read EVERY tick while the watched source is moving (so a streaming preview
/// follows at the full panel rate and looks smooth), but once it has been static for a short
/// grace period, skip progressively more ticks (capped) so re-reading an UNCHANGING but large
/// source doesn't burn CPU every tick. Any observed change snaps the cadence back to every-tick.
///
/// Usage, once per tick:
///   if (!poll.shouldRead()) return;     // this tick is skipped
///   const bool changed = doTheRead();   // the (potentially expensive) work
///   poll.observe(changed);              // feed the result back
/// and poll.reset() when the watched target switches.
class AdaptivePoll {
 public:
  /// True on ticks we should (re-)read; otherwise consumes one tick of the current skip gap.
  bool shouldRead() {
    if (skip_ticks_ > 0) {
      --skip_ticks_;
      return false;
    }
    return true;
  }

  /// Feed back whether the read observed a change. A change snaps to every-tick reads; a run
  /// of no-change grows the skip gap (after a grace period, capped).
  void observe(bool changed) {
    if (changed) {
      idle_streak_ = 0;
      skip_ticks_ = 0;
      return;
    }
    ++idle_streak_;
    if (idle_streak_ > kIdleGrace) {
      const unsigned over = idle_streak_ - kIdleGrace;
      skip_ticks_ = over < kMaxSkip ? over : kMaxSkip;
    }
  }

  /// Reset to the eager (every-tick) state — call when the watched target changes.
  void reset() {
    idle_streak_ = 0;
    skip_ticks_ = 0;
  }

 private:
  static constexpr unsigned kIdleGrace = 8;  ///< no-change ticks before backing off (~0.4 s @20 Hz)
  static constexpr unsigned kMaxSkip = 10;   ///< cap on the skip gap (~0.5 s @20 Hz)
  unsigned idle_streak_ = 0;                 ///< consecutive unchanged reads
  unsigned skip_ticks_ = 0;                  ///< ticks remaining to skip before the next read
};

}  // namespace toolbox_preview
