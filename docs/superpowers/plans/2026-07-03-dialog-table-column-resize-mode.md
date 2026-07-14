# Implementation plan: per-column resize-mode hint for dialog tables

- **Date:** 2026-07-03 (facts refreshed 2026-07-14 after PJ4#356 and plugins #197/#199/#200 merged)
- **Design:** [`specs/2026-07-03-dialog-table-column-resize-mode-design.md`](../specs/2026-07-03-dialog-table-column-resize-mode-design.md)
- **Repos, in order:** `plotjuggler_sdk` → `PJ4` → `pj-official-plugins`

## Goal

Let a picker dialog declare, per column, its Qt header resize mode, so the name
column can `Stretch` while short data columns (`Encoding`, `Msg Count`, `DataType`,
`Codec`, `Resolution`, `Size`, `Date`) `ResizeToContents`. Today the host's
`installTreeLikeHeader()` hardcodes col0=`Stretch` and cols1..n=`Interactive@96px`
with no override path.

The change is additive and header-only on the protocol side (no compiled-ABI break);
the only real coupling is the SDK version pin that PJ4 and the plugins compile against.

## Preconditions / facts (verified)

- `PJ::WidgetData` (namespace `PJ`) exposes `toJson()`; writers mutate via
  `entry(name)["key"] = ...` and follow the `setRowColor`/`setCellTooltip` object-map
  idiom (a JSON object keyed by stringified index).
- `PJ::WidgetDataView` reader parses numeric keys with `parseNumber<int>` and returns
  `std::optional<...>` from typed accessors (mirror `rowColors()`).
- Writer test idiom (`widget_data_test.cpp`):
  `static json parse(const WidgetData& wd){ return json::parse(wd.toJson()); }`,
  `using PJ::WidgetData`.
- Reader test idiom (`widget_data_view_test.cpp`): `PJ::WidgetDataView v(R"(json)")`.
- Host: `PJ4/pj_dialog_host/src/widget_binding.cpp` — free function
  `installTreeLikeHeader(QTableWidget*)`; the `QTableWidget` branch of
  `applyToWidget()` sets headers, forces `NoEditTriggers` (PJ4#356) and calls
  `installTreeLikeHeader(tw)`. (Line numbers deliberately omitted — #356 reshuffled
  the file; anchor by symbol.)
- Enum wire values are fixed by design: `Interactive=0, Fixed=1, Stretch=2,
  ResizeToContents=3` (must match the reader/host switch; do not renumber).

---

## Phase A — `plotjuggler_sdk` protocol (TDD)

Directory: `pj_plugins/dialog_protocol/include/pj_plugins/`.

### A1. Shared enum header
- **Change:** add `PJ::ColumnResizeMode { Interactive=0, Fixed=1, Stretch=2, ResizeToContents=3 }`
  in a small header included by both `sdk/widget_data.hpp` and `host/widget_data_view.hpp`
  (so neither side depends on the other). Namespace `PJ`, `enum class`, explicit values.
- **Verify:** both headers still compile (`ctest` build of the dialog_protocol tests).

### A2. Writer `setColumnResizeMode` (test first)
- **Test** (`tests/widget_data_test.cpp`): call
  `wd.setColumnResizeMode("t", 0, PJ::ColumnResizeMode::Stretch)` and
  `...("t", 2, PJ::ColumnResizeMode::ResizeToContents)`; assert
  `parse(wd)["t"]["column_resize_modes"] == {{"0",2},{"2",3}}`. Add a second-call-overwrites
  assertion (set col 0 again → value replaced, object not duplicated).
- **Impl** (`sdk/widget_data.hpp`): per the design snippet — lazily create the
  `column_resize_modes` object, `m[std::to_string(column)] = static_cast<int>(mode)`,
  return `*this`.

### A3. Reader `columnResizeModes` (test first)
- **Test** (`tests/widget_data_view_test.cpp`): feed
  `{"t":{"column_resize_modes":{"0":2,"2":3,"5":99}}}`; assert the accessor returns
  `{{0,Stretch},{2,ResizeToContents}}` — the out-of-range `99` is **skipped**. Assert a
  table with no key returns `std::nullopt`.
- **Impl** (`host/widget_data_view.hpp`): `columnResizeModes(name) ->
  std::optional<std::vector<std::pair<int, ColumnResizeMode>>>`; iterate the object,
  key→col via `parseNumber<int>`, value(int)→enum only if in `[0,3]`, else skip. Return
  `nullopt` when the key is absent (parallels `rowColors()`).
- **Verify:** `ctest --test-dir build -R widget_data` green.

### A4. Cut SDK version
- Ships with the next SDK release tag (0.17.0 line at the time of this refresh —
  upstream took 0.16.x; coordinate the exact number with the SDK repo).

---

## Phase B — `PJ4` host consumption

File: `PJ4/pj_dialog_host/src/widget_binding.cpp`.

### B1. `toQtResizeMode` free function
- Add the 4-case switch mapping `PJ::ColumnResizeMode` → `QHeaderView::ResizeMode`
  (default `Interactive`). Place near `installTreeLikeHeader`.

### B2. Apply hints after `installTreeLikeHeader`
- In the `QTableWidget` branch, immediately **after** `installTreeLikeHeader(tw)`:
  ```cpp
  if (auto modes = view.columnResizeModes(name)) {
    auto* header = tw->horizontalHeader();
    for (const auto& [col, mode] : *modes) {
      if (col >= 0 && col < header->count()) {
        header->setSectionResizeMode(col, toQtResizeMode(mode));
      }
    }
  }
  ```
- **Semantics:** hints override the hardcoded default only for named columns; unnamed
  columns keep `installTreeLikeHeader`'s value; hint-free tables unchanged; a hint for a
  not-yet-existing column is a harmless no-op (range check).
- Bump PJ4's SDK pin to the Phase-A tag.
- **Verify:** build PJ4; open the MCAP load dialog; confirm `Encoding`/`Msg Count` hug
  content while `Channel name` stretches.

---

## Phase C — `pj-official-plugins` rollout

Add `setColumnResizeMode` calls next to each picker's existing `setTableHeaders`, per the
design's column-layout table. Bump `SDK_VERSION` + submodule via
`python3 scripts/bump_core_version.py <new-version>`.

### C1. MCAP `tableWidget` (`data_load_mcap/mcap_dialog.hpp`)
- 0 Channel name = **Stretch**; 1 Schema, 2 Encoding, 3 Msg Count = **ResizeToContents**.

### C2. foxglove `topicsList` / pj_bridge `topicsList`
- foxglove (3 cols, set in code): 0 Topic Name = **Stretch**; 1 DataType, 2 Encoding = **ResizeToContents**.
- pj_bridge (2 cols): 0 Topic Name = **Stretch**; 1 DataType = **ResizeToContents**.

### C3. ros2 `listRosTopics`
- 0 Topic = **Stretch**; 1 Datatype = **ResizeToContents**.

### C4. webrtc `camerasList`
- 0 Camera = **Stretch**; 1 Codec, 2 Resolution = **ResizeToContents**.

### C5. mosaico `seqTable` / `topicTable`
- seqTable: 0 Name = **Stretch**; 1 Date, 2 Size = **ResizeToContents**.
- topicTable: 0 Name = **Stretch**; 1 Size = **ResizeToContents**.

### C6. Version bump + build
- `scripts/bump_core_version.py --check` guards SDK_VERSION↔submodule agreement.
- `./build.sh` each affected plugin; visual confirmation of each picker.

---

## Done criteria

- [ ] SDK: enum + writer + reader landed, `widget_data` tests green, new tag cut.
- [ ] PJ4: `toQtResizeMode` + apply-block landed, SDK pin bumped, MCAP dialog visually correct.
- [ ] Plugins: 7 picker tables carry resize hints, `SDK_VERSION` bumped, `--check` passes.
- [ ] Every hint-free table in every dialog is visually unchanged (backward-compat proof).

## Notes

- MCAP `Schema` uses `ResizeToContents`; a long `sensor_msgs/msg/PointCloud2` will be wide
  but non-stretching — accepted in design review.
- Enum wire values are load-bearing across three repos; changing them silently desyncs the
  host switch from the writer. Treat `{Interactive,Fixed,Stretch,ResizeToContents}={0,1,2,3}`
  as frozen.
