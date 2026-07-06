# data_load_mcap: always-include /tf and /tf_static

## Problem

`data_load_mcap`'s topic-picker dialog treats every MCAP channel identically:
the user checks/unchecks rows, and only checked topics get parsed and pushed
into the ObjectStore. When a user narrows their selection instead of using
"Select All" — the common case when they only care about a handful of
signals — `/tf` and `/tf_static` can end up unchecked. Since PJ4's Scene3D
needs the transform tree to place `PointCloud`/`Mesh3D`/other 3D objects,
this silently breaks 3D rendering with no obvious cause.

The plugin is meant to be format-agnostic — it must not simply hardcode
`if topic == "/tf"` as a ROS special case baked into otherwise-generic
loading logic.

## Mechanism: generic always-include whitelist, keyed by (topic, encoding)

A small static table of `{topic, encoding}` pairs, matched against
`ChannelInfo{topic, encoding}` — fields already populated in `analyzeFile()`
directly from the MCAP channel summary (`channel_ptr->topic`,
`channel_ptr->messageEncoding`). No parser instantiation, no schema
introspection, no ROS awareness in the mechanism itself:

```cpp
struct AlwaysIncludeRule {
  std::string topic;
  std::string encoding;
};

const std::vector<AlwaysIncludeRule> kAlwaysIncludeRules = {
  {"/tf", "cdr"},
  {"/tf_static", "cdr"},
};
```

**Encoding value, verified against real ROS 2 bags:** MCAP's `Channel`
record carries `message_encoding` (the wire/serialization format of the
message bytes) separately from the `Schema` record's `encoding` (the
schema-definition language). For `tf2_msgs/msg/TFMessage` on real rosbag2
mcap recordings (checked with the `mcap` CLI against
`~/ws_plotjuggler/DATA/amcl_test_bag.mcap` and others):

- Channel `messageEncoding` = `cdr` — this is what `ChannelInfo.encoding`
  actually stores (`mcap_dialog.hpp` prefers `channel_ptr->messageEncoding`
  over the schema's encoding whenever the channel's own field is non-empty).
- Schema `encoding` = `ros2msg` — the message-definition-language tag shown
  by `mcap info`'s bracketed suffix; not what `ChannelInfo.encoding` holds
  for a normally-written ROS 2 bag.

So the whitelist rule matches on `"cdr"`, not `"ros2msg"`.

The mechanism itself is generic: `kAlwaysIncludeRules` is just data. A
future convention (a different middleware's own transform-tree topic, or
some other rendering-dependency) could add a row without touching the
enforcement logic below. ROS's `/tf`/`/tf_static` are simply the first (and
for now, only) entries.

## Enforcement: pre-checked, cannot uncheck

Channels matching a whitelist rule (with `msg_count > 0`) are:

1. Unconditionally re-inserted into `selected_topics_` at every mutation
   point: `analyzeFile()`'s default-select path, `onSelectionChanged`, and
   both `btnSelectAll`/`btnDeselectAll` handlers in `onClicked`.
2. Rendered in `widget_data()` as both selected and disabled — reusing the
   existing `setSelectedRows`/`setDisabledRows` calls already used to gray
   out zero-message channels — so the checkbox/row cannot be toggled by the
   user.
3. Given a `setCellTooltip` on the topic-name cell explaining why: "Always
   loaded — required for 3D transform rendering."

The correctness guarantee lives at the model layer (`selected_topics_`),
not the view: even if the widget's visual state were ever wrong, every
handler re-asserts whitelist membership before the dialog's state is
considered final, and `mcap_source.cpp`'s existing `topicFilter` (unchanged)
reads only from `selected_topics_`.

## Known limitation, not fixed here

`tableWidget` has `sortingEnabled=true`. Prior work already found that
`setSelectedRows`/`setDisabledRows` are index-based and desync visually
after a native column-header sort (the QUiLoader-loaded table reorders rows
without notifying the plugin), because `data_load_mcap` was deferred from
the fix applied to the ROS2/WebRTC pickers (which moved to the text-keyed
`setSelectedItems`, an SDK method that already exists). A full fix for the
disabled-rows side needs a new text-keyed `setDisabledItems` at the SDK
level (`plotjuggler_sdk` + PJ4), a larger cross-repo change.

Because this feature's guarantee is enforced in `selected_topics_`
regardless of what the view displays, a post-sort desync can only cause a
**cosmetic** glitch (the locked look landing on the wrong row momentarily)
— never a dropped `/tf`/`/tf_static` topic. This design bundles the cheap,
already-available half of the fix (switching *selection* restore from
`setSelectedRows` to `setSelectedItems`, matching what PR #188 did for
ros2/webrtc) since it's directly relevant to a newly-added kind of
"selected" state, but leaves the disabled-rows index limitation as a
documented, pre-existing gap rather than pulling in the larger SDK change.

## Data flow

1. `analyzeFile()` builds `all_channels_` from the MCAP summary.
2. Any channel matching `kAlwaysIncludeRules` (with `msg_count > 0`) is
   force-inserted into `selected_topics_`.
3. `widget_data()` marks those rows selected + disabled every render, with
   a tooltip.
4. `onSelectionChanged` / `onClicked` (`btnSelectAll`/`btnDeselectAll`)
   re-assert whitelist membership after applying the user's requested
   change.
5. `loadConfig()` (restoring a previously-saved layout) also re-asserts
   whitelist membership after populating `selected_topics_` from the saved
   JSON, so an old saved config that predates this feature (and therefore
   omits `/tf`) still gets it forced back in.
6. `mcap_source.cpp`'s `topicFilter` (unchanged) reads from
   `selected_topics_` — whitelisted topics are always parsed and pushed,
   feeding Scene3D via the existing `kFrameTransforms` classification in
   `parser_ros`.

## Testing

`mcap_dialog_test.cpp` was deleted in PR #144 when its old tests (which
asserted the presence of widgets removed in that PR) went stale, and was
never replaced — `McapDialog` currently has zero direct unit coverage. The
CMake wiring for the removed target is recoverable from git history
(`git show 7f024e2 -- data_load_mcap/CMakeLists.txt`) and will be restored
with the same shape (links `plotjuggler_sdk::plugin_sdk`, `nlohmann_json`,
`LZ4::lz4_static`, `zstd::libzstd_static`, `GTest::gtest_main`).

New coverage needed:

- Whitelist matching: exact `(topic, encoding)` hits match; near-misses
  (right topic wrong encoding, right encoding wrong topic) do not.
- `analyzeFile()` on a synthetic MCAP with a `cdr`-encoded `/tf` channel:
  the topic ends up in `selected_topics_` even when `loadConfig()` supplies
  a saved selection that omits it.
- `onClicked(btnDeselectAll)` and `onSelectionChanged` with `/tf` excluded
  from the reported `selected` list still leave it in `selected_topics_`.
- Zero-msg-count `/tf` channel (edge case) is *not* force-included — matches
  the existing "nothing to load" rule for empty channels.
- A small fixture MCAP with a `cdr`-encoded `/tf` topic will be added to
  `test_data/`, following the existing pattern in
  `generate_verification_mcaps.py`.

## Non-goals

- `data_stream_ros2` / `data_stream_foxglove_bridge`'s demand-driven
  per-topic subscription work (`feat/topic-subscription`, unmerged) has the
  same underlying risk for live streaming — a "required" topic may never be
  dragged into a plot, so it's never subscribed. That's a separate
  plugin/branch not yet merged; flagged as a related follow-up, out of
  scope here.
- No glob/regex topic-name matching — exact string match only.
- No user-facing UI to edit the whitelist — it's a compiled-in table.
