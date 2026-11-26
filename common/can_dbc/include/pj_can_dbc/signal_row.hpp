#pragma once

// SignalRowBuilder: turns one decoded CAN frame's signals into the flat NamedFieldValue row
// appendRecord() wants: the numeric field for every signal (name unchanged), plus a
// "<signal>_label" text field for any signal whose DBC carries a VAL_ value table (so the State
// Transitions view can show the decoded label). A raw value with no matching table entry still
// gets a text field holding the number as text.
//
// The text field is "<signal>_label", or "<signal>_label_2", "_3", ... when the message already
// uses that name for a real signal (a DBC may pair "Foo" with a genuine "Foo_label") or for
// another signal's label, so a row never holds two fields with the same name. CanDecoder fixes
// the names at DBC load; this builder just writes DecodedSignal::label_name.

#include <array>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/span.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <vector>

namespace pj_can_dbc {

/// Builds one appendRecord() row per decoded frame. One instance per import, one build() per
/// frame, so the field/buffer vectors' allocations are amortized.
class SignalRowBuilder {
 public:
  /// The returned span, and every std::string_view inside the values it points to, is valid only
  /// until the next build() on this instance (or its destruction): pass it straight to
  /// appendRecord().
  PJ::Span<const PJ::sdk::NamedFieldValue> build(const std::vector<DecodedSignal>& signals);

 private:
  // Never clear()ed: grows to the largest frame's field count, and build() overwrites slots in
  // place via NamedFieldValue::name.assign(), so a steady stream of same-size frames allocates no
  // names. Only the first N slots returned by the latest build() are current.
  std::vector<PJ::sdk::NamedFieldValue> fields_;
  // One text buffer per signal, sized up front each call so a mid-call resize never relocates a
  // buffer a string_view in `fields_` already points to.
  std::vector<std::array<char, 24>> fallback_buffers_;
};

}  // namespace pj_can_dbc
