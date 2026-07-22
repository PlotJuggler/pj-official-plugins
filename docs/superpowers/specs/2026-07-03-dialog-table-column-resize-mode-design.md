# Design: per-column resize-mode hint for dialog tables

- **Date:** 2026-07-03 (facts refreshed 2026-07-14 after PJ4#356 and plugins #197/#199/#200 merged)
- **Status:** Approved (design) — pending implementation plan
- **Anchor repo:** `pj-official-plugins` (picker dialogs)
- **Also touches:** `plotjuggler_sdk` (protocol), `PJ4` (dialog host)

## Problem

The picker dialogs (topic / channel / camera / sequence selection tables) have been
unified on selection model, vertical-header visibility, and sorting via their `.ui`
files (edit-triggers need no per-`.ui` copy: since PJ4#356 the host forces every
protocol table read-only). The one axis that **cannot** be expressed in a `.ui` file is
per-column resize behavior.

Column sizing is owned entirely by the host: `PJ4/pj_dialog_host/src/widget_binding.cpp`
runs `installTreeLikeHeader()` on every table that receives `setTableHeaders()`. It
hardcodes **column 0 → `Stretch`** and **columns 1..n → `Interactive` at a fixed 96 px**,
and it overrides whatever the `.ui` sets. There is no way, today, for a plugin to say
"this short column should hug its content" (e.g. MCAP `Encoding`, `Msg Count`).

Desired idiom for the pickers: the **name column is widest and stretches**; the short
data columns **resize to their content**.

## Goals

- Let a plugin declare, per column, the Qt resize mode for a dialog table.
- Apply the pickers' idiom: name column `Stretch`, short columns `ResizeToContents`.
- Fully backward compatible: tables that send no hint keep today's behavior.

## Non-goals (YAGNI)

- No vector-form convenience overload — per-column setter only.
- No per-column min/max width control.
- No change to mosaico sorting — scope decision: its tables already re-sort via the
  plugin-owned `onHeaderClicked` sorter (`table_sort.h`), and since PJ4#356 the host
  translates index-keyed state under built-in sort anyway, so switching mosaico to
  `sortingEnabled` is possible but out of scope here.

## API decision

Per-column setter mirroring `QHeaderView::setSectionResizeMode(int, ResizeMode)` — chosen
over a vector form for fidelity to Qt (project convention: new SDK methods mirror the
nearest Qt class). Only columns that differ from the host default need to be set.

```cpp
enum class ColumnResizeMode { Interactive = 0, Fixed = 1, Stretch = 2, ResizeToContents = 3 };

wd.setColumnResizeMode("tableWidget", 0, PJ::ColumnResizeMode::Stretch);
wd.setColumnResizeMode("tableWidget", 2, PJ::ColumnResizeMode::ResizeToContents);
// JSON on the widget entry: "column_resize_modes": {"0":2, "2":3}
```

## Design

### 1. Protocol — `plotjuggler_sdk/pj_plugins/dialog_protocol`

- **Enum** `PJ::ColumnResizeMode` lives in a small shared header included by both the
  writer (`sdk/widget_data.hpp`) and the reader (`host/widget_data_view.hpp`), so neither
  side depends on the other's header.
- **Writer** — `sdk/widget_data.hpp`, following the existing `setRowColor`/`setCellTooltip`
  object-map pattern:

  ```cpp
  WidgetData& setColumnResizeMode(std::string_view name, int column, ColumnResizeMode mode) {
    auto& e = entry(name);
    auto& m = e["column_resize_modes"];
    if (!m.is_object()) { m = nlohmann::json::object(); }
    m[std::to_string(column)] = static_cast<int>(mode);
    return *this;
  }
  ```

- **Reader** — `host/widget_data_view.hpp`, following the existing `rowColors()` reader:

  ```cpp
  std::optional<std::vector<std::pair<int, ColumnResizeMode>>>
  columnResizeModes(std::string_view name) const;
  // iterate the object; key -> col via parseNumber<int>; value(int) -> ColumnResizeMode
  ```

  Out-of-range enum ints are skipped (defensive, matches reader style elsewhere).

Both additions are inline header-only → additive, no compiled-ABI break.

### 2. Host — `PJ4/pj_dialog_host/src/widget_binding.cpp`

In the `QTableWidget` branch, after `installTreeLikeHeader(tw)`, add an idempotent block:

```cpp
static QHeaderView::ResizeMode toQtResizeMode(PJ::ColumnResizeMode m) {
  switch (m) {
    case PJ::ColumnResizeMode::Interactive:      return QHeaderView::Interactive;
    case PJ::ColumnResizeMode::Fixed:            return QHeaderView::Fixed;
    case PJ::ColumnResizeMode::Stretch:          return QHeaderView::Stretch;
    case PJ::ColumnResizeMode::ResizeToContents: return QHeaderView::ResizeToContents;
  }
  return QHeaderView::Interactive;
}

if (auto modes = view.columnResizeModes(name)) {
  auto* header = tw->horizontalHeader();
  for (const auto& [col, mode] : *modes) {
    if (col >= 0 && col < header->count()) {
      header->setSectionResizeMode(col, toQtResizeMode(mode));
    }
  }
}
```

**Semantics:** hints override the hardcoded default *only* for the columns they name.
Unnamed columns keep `installTreeLikeHeader`'s value; hint-free tables are unchanged.
Gated by `header->count() > 0` via the range check, so a hint delivered before columns
exist is a harmless no-op (plugins send headers + hints together each build anyway).

### 3. Plugins — the picker rollout (this repo)

Add `setColumnResizeMode` calls next to each picker's existing `setTableHeaders`:

| Picker (objectName) | Column resize layout |
|---|---|
| mcap `tableWidget` | 0 Channel name = **Stretch**; 1 Schema, 2 Encoding, 3 Msg Count = **ResizeToContents** |
| foxglove `topicsList` | 0 Topic Name = **Stretch**; 1 DataType, 2 Encoding = **ResizeToContents** (3 cols, set in code) |
| pj_bridge `topicsList` | 0 Topic Name = **Stretch**; 1 DataType = **ResizeToContents** |
| ros2 `listRosTopics` | 0 Topic = **Stretch**; 1 Datatype = **ResizeToContents** |
| webrtc `camerasList` | 0 Camera = **Stretch**; 1 Codec, 2 Resolution = **ResizeToContents** |
| mosaico `seqTable` | 0 Name = **Stretch**; 1 Date, 2 Size = **ResizeToContents** |
| mosaico `topicTable` | 0 Name = **Stretch**; 1 Size = **ResizeToContents** |

Note: MCAP `Schema` uses `ResizeToContents`; a long `sensor_msgs/msg/PointCloud2` will be
wide but non-stretching — accepted (approved in design review).

### 4. Rollout order & versioning

1. Add enum + writer + reader to `plotjuggler_sdk` (additive, header-only); goes out in
   the next SDK release tag (0.17.0 at the time of this refresh — upstream took the
   0.16.x range).
2. `PJ4` host consumes the reader; bump PJ4's SDK pin.
3. Plugins call the writer; bump this repo's `SDK_VERSION` + submodule via
   `scripts/bump_core_version.py`.

Because the API is header-only inline, the only coupling is the version pin: the new
method must exist in the SDK revision that PJ4 and the plugins compile against.

## Testing

- **SDK unit test** (`dialog_protocol/tests/widget_data_view_test.cpp`): round-trip
  `column_resize_modes` writer → JSON → reader, including an out-of-range enum int that
  is skipped.
- **Host**: interactive check in PJ4 — open the MCAP load dialog; confirm `Encoding` and
  `Msg Count` hug their content while `Channel name` stretches to fill.
- **Plugins**: `./build.sh` the affected plugins; visual confirmation of each picker.

## Rejected alternatives

- **Keep fixed 96 px** — leaves short columns wastefully wide; doesn't meet the ask.
- **`ResizeToContents` globally in the host** — simplest, but reverses the host author's
  deliberate "draggable dividers" choice for *all* header tables, not just pickers.
- **Vector-form API** — fits the existing `setTableHeaders` shape, but the per-column form
  mirrors `QHeaderView` more faithfully (chosen).
