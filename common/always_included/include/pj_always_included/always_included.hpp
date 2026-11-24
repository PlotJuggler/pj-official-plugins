// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

/// @file always_included.hpp
/// @brief The channels a topic-selecting plugin always loads or subscribes, whatever the user
///  selected: 3D rendering (Scene3D's transform tree) silently breaks without them. Shared by the
///  MCAP loader and the streamers (PlotJuggler Bridge, Foxglove Bridge, ROS 2) so they agree.
///
///  Keyed on message type, never on topic name: topic naming is not consistent across producers
///  (Foxglove's own foxglove.FrameTransform has no fixed topic name; real recordings use "tf",
///  "/tf" or something else entirely). The mechanism is generic; kRules is just its data.
///
///  Who decides a channel is present differs:
///   - the MCAP loader forces a matching channel only when it holds messages (it knows the
///     file's counts; an empty channel is not selectable at all);
///   - a streamer forces every matching channel the server advertises, since it cannot know
///     counts before subscribing. A matching channel the server does not advertise is never
///     invented.
///  In a dialog, a forced row is shown selected, disabled and carries kTooltip; Deselect All, a
///  host-reported selection or a saved selection that omits it never drops it. A forced channel
///  is part of the selection the dialog saves, so a host that subscribes exactly the saved
///  selection (one without demand subscriptions) subscribes it too. A streamer that uses the
///  saved selection as its advertise filter ignores the forced entries: a selection of forced
///  channels only still means "no filter". locked_table.hpp is the dialogs' shared table.

#include <array>
#include <string_view>

namespace PJ::AlwaysIncluded {

/// A (schema name, message encoding) pair. The encoding is the channel's MESSAGE encoding
/// (MCAP channel `messageEncoding`, Foxglove channel `encoding`): "cdr" for every ROS 2 source.
struct Rule {
  std::string_view schema_name;
  std::string_view encoding;
};

/// Only the types a PlotJuggler parser turns into the canonical FrameTransforms object.
inline constexpr std::array<Rule, 2> kRules = {{
    {"tf2_msgs/msg/TFMessage", "cdr"},
    {"foxglove.FrameTransform", "protobuf"},
}};

/// The tooltip of a forced (locked) row, the same in every dialog.
inline constexpr std::string_view kTooltip = "Always loaded: required for 3D transform rendering.";

/// True when a channel of this type is always loaded/subscribed. Exact, case-sensitive match.
[[nodiscard]] constexpr bool matches(std::string_view schema_name, std::string_view encoding) {
  for (const auto& rule : kRules) {
    if (schema_name == rule.schema_name && encoding == rule.encoding) {
      return true;
    }
  }
  return false;
}

/// matches() for a ROS 2 topic of type `type` (PlotJuggler Bridge, ROS 2 streamer): every ROS 2
/// topic is CDR, so its own encoding field is not consulted.
[[nodiscard]] constexpr bool matchesRos2(std::string_view type) {
  return matches(type, "cdr");
}

}  // namespace PJ::AlwaysIncluded
