#pragma once

// SignalRowBuilder: turns one decoded CAN frame's signals into the flat
// NamedFieldValue row appendRecord() wants -- the numeric field for every
// signal (name unchanged), plus a "<signal>_label" text field for any
// signal whose DBC carries a VAL_ value table (so PJ4's State Transitions
// view can show the decoded label instead of a bare number). A raw value
// with no matching table entry still gets a "<signal>_label" field, holding
// the number as text -- callers see something for every value, not a
// silently-missing field.
//
// Shared by data_load_blf, data_load_mf4 and data_load_candump so the
// "<x>_label" convention -- and its collision risk with a signal that is
// itself genuinely named "<x>_label" in the DBC -- is defined exactly once.
// See each plugin's README for that collision note.

#include <array>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/span.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <vector>

namespace pj_can_dbc {

/// Builds one appendRecord() row per decoded frame. Reused across an entire
/// import (one instance per import, one build() call per frame): amortizes
/// the field/buffer vectors' allocations instead of paying for them per
/// frame.
class SignalRowBuilder {
 public:
  /// Returns the row for `signals`. The returned span, and every
  /// std::string_view inside the PJ::sdk::NamedFieldValue values it points
  /// to, is valid only until the next call to build() on this instance (or
  /// its destruction) -- pass it straight to appendRecord() before building
  /// the next row.
  PJ::Span<const PJ::sdk::NamedFieldValue> build(const std::vector<DecodedSignal>& signals);

 private:
  // Never clear()ed: only grows across calls (up to the largest frame's
  // field count seen so far), and build() overwrites slots in place via
  // NamedFieldValue::name.assign() rather than pushing fresh ones -- so a
  // steady-state stream of frames with the same signal count does no
  // per-frame name allocation. Only the first N slots returned by the most
  // recent build() are current; anything beyond that is stale leftover.
  std::vector<PJ::sdk::NamedFieldValue> fields_;
  // One text buffer per signal (upper bound: at most one is used per
  // out-of-table signal), sized up front each call so a mid-call resize
  // never relocates a buffer a string_view in `fields_` already points to.
  std::vector<std::array<char, 24>> fallback_buffers_;
};

}  // namespace pj_can_dbc
