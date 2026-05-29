// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace mosaico {

// PJ3 SequencePicker RangeFilter interval semantics (sequence_panel.cpp:344-415).
// A sequence spanning [seq_min, seq_max] (epoch-ns) passes when it intersects the
// picked range [from_ns, to_ns]. Zero on a bound means "unbounded" on that side;
// a zero seq bound is treated as "unknown" and never excludes.
//
// A dateless sequence (both bounds 0) is shown only when no date constraint is
// active. Once a range is active it is hidden: a dateless sequence can't be
// confirmed in-range, and surfacing it (displayed as a 1970 epoch date) is a
// false positive. Deliberate improvement over PJ3, which never hid these.
inline bool dateFilterMatches(std::int64_t seq_min, std::int64_t seq_max, std::int64_t from_ns, std::int64_t to_ns) {
  if (seq_min == 0 && seq_max == 0) {
    return from_ns == 0 && to_ns == 0;
  }
  if (from_ns != 0 && seq_max != 0 && seq_max < from_ns) {
    return false;
  }
  if (to_ns != 0 && seq_min != 0 && seq_min > to_ns) {
    return false;
  }
  return true;
}

}  // namespace mosaico
