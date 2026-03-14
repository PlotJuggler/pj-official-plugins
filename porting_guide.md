# Plugin Porting Guide

Reference document for porting PlotJuggler plugins to the new SDK. Based on lessons learned porting the CSV plugin (the only fully verified plugin so far). Other plugins (Parquet, ULog, MCAP, Protobuf, ROS, ZMQ, MQTT, Foxglove Bridge, PJ Bridge) are ported but **not yet validated end-to-end** — expect similar issues.

> **RULE: Full feature parity is mandatory.** When porting a plugin, every feature
> of the original must be replicated — no skipping, no "good enough" shortcuts.
> If a feature cannot be implemented due to SDK limitations, document it explicitly
> as a known limitation with a workaround plan. Never mark a port as complete
> when features are silently dropped. This includes: string data columns, user-facing
> warning dialogs, detailed error reporting, and all dialog interactions.

> **RULE: 1:1 dialog replication is non-negotiable.** This is not optional, not
> "over-engineering", and must not be postponed or simplified:
>
> - **Copy the original `.ui` file exactly.** Only modify what strictly cannot work
>   (e.g., custom widget classes whose headers don't exist in this codebase).
>   Everything else — layout, sizing, spacing, naming, properties — stays identical.
> - **Use real `.ui` files** editable in Qt Creator, auto-embedded into a
>   `constexpr char[]` header via CMake's `pj_embed_ui()`. Do NOT write inline
>   XML strings manually. Plugins must NOT depend on Qt for UI loading.
> - **Smart dialog plugins**: streaming source dialogs that have dynamic topic
>   discovery (PJ Bridge, Foxglove, MQTT) must own the connection logic inside
>   the dialog plugin itself, replicating the original Connect → discover → select
>   → subscribe flow.
> - If the dialog SDK lacks support for a widget type used by the original (e.g.,
>   QTreeWidget), **extend the SDK** — do not downgrade to a simpler widget.

---

## 1. SDK Plugin Patterns

### FileSourceBase (one-shot file importers)

```
Header: pj_base/include/pj_base/sdk/data_source_patterns.hpp
Example: pj_plugins/examples/mock_file_source.cpp
```

Override:
- `uint64_t extraCapabilities() const` — e.g. `kCapabilityDirectIngest | kCapabilityHasDialog`
- `Status importData()` — do all work here; `writeHost()` and `runtimeHost()` are available
- `std::string saveConfig() const` / `Status loadConfig(std::string_view json)`

The base class manages the state machine automatically: Idle → Starting → importData() → Stopped/Failed.

Config convention: file importers receive `{"filepath":"/path/to/file"}` via `loadConfig()`. The manifest must include `"file_extensions":[".csv",".tsv"]` for the host file dialog.

### StreamSourceBase (long-lived streaming sources)

```
Header: pj_base/include/pj_base/sdk/data_source_patterns.hpp
Example: pj_plugins/examples/mock_source_with_dialog.cpp
```

Override:
- `uint64_t extraCapabilities() const`
- `Status onStart()` — open connections
- `Status onPoll()` — **must not block**; drain available data and return
- `void onStop()` — **must be idempotent**; close connections

### MessageParserPluginBase (message decoders)

```
Header: pj_base/include/pj_base/sdk/message_parser_plugin_base.hpp
Example: pj_plugins/examples/mock_json_parser.cpp
```

Override:
- `Status parse(Timestamp ts, Span<const uint8_t> payload)` — **pure virtual**
- `Status bindSchema(std::string_view type_name, Span<const uint8_t> schema)` — optional

Manifest **must** include `"encoding"` key (e.g. `"json"`, `"cdr"`, `"protobuf"`).

### Capability Flags

```cpp
kCapabilityFiniteImport     // auto-added by FileSourceBase
kCapabilityContinuousStream // auto-added by StreamSourceBase
kCapabilityDirectIngest     // plugin writes data via writeHost()
kCapabilityDelegatedIngest  // plugin pushes raw bytes to host-side parsers
kCapabilityHasDialog        // plugin provides a dialog UI
kCapabilitySupportsPause    // must override pause()/resume()
```

### Export Macros

```cpp
PJ_DATA_SOURCE_PLUGIN(ClassName, R"({"name":"...","version":"1.0.0"})")
PJ_MESSAGE_PARSER_PLUGIN(ClassName, R"({"name":"...","version":"1.0.0","encoding":"..."})")
PJ_DIALOG_PLUGIN(DialogClassName)  // if plugin has a dialog
```

---

## 2. Data Write API — Critical Details

```
Header: pj_base/include/pj_base/sdk/plugin_data_api.hpp
```

### ValueRef preserves native types — DO NOT cast to double

```cpp
using ValueRef = std::variant<NullValue,
    float, double,
    int8_t, int16_t, int32_t, int64_t,
    uint8_t, uint16_t, uint32_t, uint64_t,
    bool, std::string_view>;
```

**PITFALL (hit during porting):** The initial protobuf and JSON parsers cast everything to `double`, losing precision for int64/uint64 values. Push native types directly:

```cpp
// WRONG — loses precision for large integers
out.push_back({name, static_cast<double>(value.get<int64_t>())});

// CORRECT — preserves full precision
out.push_back({name, value.get<int64_t>()});
```

### NamedFieldValue uses std::string — safe by default

```cpp
struct NamedFieldValue {
  std::string name;   // owned string (was string_view, changed for safety)
  ValueRef value;
};
```

The `name` field is now `std::string` (previously `string_view`). This eliminates the dangling reference foot-gun that affected early plugin ports. You can safely build field names via string concatenation:

```cpp
std::vector<PJ::sdk::NamedFieldValue> fields;
fields.push_back({prefix + "/" + key, value});  // safe — name is owned
writeHost().appendRecord(ts, PJ::Span(fields.data(), fields.size()));
```

### Timestamps are absolute nanoseconds

Store raw timestamps (e.g. epoch nanoseconds) in the datastore. Never subtract a base time during ingestion. Display-time subtraction belongs in the UI layer only.

```cpp
// Epoch seconds from CSV → nanoseconds for datastore
auto ts = static_cast<int64_t>(epoch_seconds * 1e9);
writeHost().appendRecord(*topic, PJ::Timestamp{ts}, fields);
```

---

## 3. Datastore Internals — Must-Know Behaviors

### 3a. ensureColumn() rejects adding columns after rows are written

```
File: pj_datastore/src/writer.cpp, lines 516-567
```

`ensureColumn()` now returns an error if the topic already has a builder with completed rows. This prevents schema fragmentation where early chunks lack columns that later chunks have (which caused segfaults when reading).

**CRITICAL: Pre-register ALL columns before writing any data.**

```cpp
auto topic = writeHost().ensureTopic(topic_name);

// Pre-register ALL fields FIRST — prevents mid-stream chunk sealing
for (const auto& col_name : all_column_names) {
  writeHost().ensureField(*topic, col_name, PJ::PrimitiveType::kFloat64);
}

// NOW write data — all columns exist from the start
for (...) {
  writeHost().appendRecord(*topic, ts, fields);
}
```

This was the root cause of the segfault in the CSV plugin with sparse data.

### 3b. finishRow() auto-fills unset columns with null

```
File: pj_datastore/src/chunk.cpp, lines 165-180
```

When `appendRecord()` is called with only some fields set, `finishRow()` automatically pads unset columns with null (zero bytes + validity bit cleared). This is correct behavior — sparse records are supported.

Example: topic has columns [x, y, z]. Record sets only x:
- Column x: value written
- Column y: null (zero bytes, validity bit = 0)
- Column z: null (zero bytes, validity bit = 0)

All columns always have the same row count. This is an invariant.

### 3c. readColumnAsDoubles() returns NaN for nulls

```
File: pj_datastore/src/chunk.cpp, lines 664-705
```

`readColumnAsDoubles()` reads raw bytes and converts to double. **For null rows, it now returns `NaN`** (not 0.0). This means consumers don't need to manually check `isNull()` — NaN values naturally propagate and are not plotted by chart panels.

```cpp
range.chunk->readColumnAsDoubles(col_index, values_span, row_start);
for (size_t i = 0; i < row_count; ++i) {
  if (std::isnan(values[i])) {
    continue;  // null value — skip
  }
  // use values[i]
}
```

Note: `readNumericAsDouble()` (single-value read) does NOT check nulls — use `isNull()` if needed.

### 3d. Chunks can have different column counts (schema evolution)

If columns were added after some chunks were sealed, early chunks have fewer columns. Before calling `readColumnAsDoubles(col_index, ...)`, check bounds:

```cpp
if (col_index >= range.chunk->columns.size()) {
  return;  // column doesn't exist in this chunk
}
```

---

## 4. Dialog SDK

```
Pattern: pj_plugins/examples/mock_source_with_dialog.cpp
WidgetData API: pj_plugins/dialog_protocol/include/pj_plugins/sdk/widget_data.hpp
```

### Supported Widgets (via WidgetData)

| Widget | set* Methods |
|--------|-------------|
| QLineEdit | `setText`, `setPlaceholder`, `setReadOnly` |
| QComboBox | `setItems`, `setCurrentIndex` |
| QCheckBox / QRadioButton | `setChecked` |
| QSpinBox | `setValue(int)`, `setRange(min, max)` |
| QDoubleSpinBox | `setValue(double)` |
| QListWidget | `setListItems`, `setSelectedItems` (vector of strings) |
| QTableWidget | `setTableHeaders`, `setTableRows` (vector of vector of strings) |
| QLabel | `setLabel` (also responds to `setText`) |
| QPushButton | `setButtonText` |
| QDialogButtonBox | `setOkEnabled` |
| QTabWidget | `setTabIndex` |
| Any widget | `setEnabled`, `setVisible` |

**NOT supported:** QTextEdit, QTableView (model-based), QTreeWidget, custom widgets. Use QTableWidget for both preview tables and multi-column selection lists (replace QTreeWidget → QTableWidget in .ui files).

### Extending the Dialog Protocol

`pj_plugins/dialog_protocol/` may need to be extended to support interactions that ported plugins require but the current SDK doesn't cover. Before adding new features, **verify that the existing interface truly cannot accomplish the goal** — sometimes a workaround using supported widgets is sufficient.

Known gaps where protocol extension may be needed:
- **Double-click on list item to accept** — old CSV plugin wires `listWidgetSeries::itemDoubleClicked` → `accept()`. The dialog SDK has no `onItemDoubleClicked` event. Could be added as a new event type in `widget_event.hpp`.
- **QTextEdit / rich text** — no binding for QTextEdit exists in `widget_binding.cpp`. Could add one following the QLineEdit pattern.
- **Raw text preview with syntax highlighting** — would require either QTextEdit support or a custom widget type.
- **User-choice message boxes** (Continue/Abort) — `reportMessage` is fire-and-forget with no return value. A blocking prompt mechanism would require a new runtime host callback (e.g. `confirmWarning(message) → bool`).

When extending, modify these files:
- `pj_plugins/dialog_protocol/include/pj_plugins/sdk/widget_event.hpp` — new event types
- `pj_plugins/dialog_protocol/src/widget_binding.cpp` — add widget type handling
- `pj_plugins/dialog_protocol/include/pj_plugins/sdk/widget_data.hpp` — new set* methods
- `pj_base/include/pj_base/data_source_protocol.h` — for new runtime host callbacks

### Dialog Implementation Checklist

1. Subclass `PJ::DialogPluginTyped`
2. Use a real `.ui` file (editable in Qt Creator) in the plugin's `ui/` directory.
   CMake auto-generates a header via `pj_embed_ui()` — `ui_content()` returns the generated
   `constexpr char[]`. No Qt dependency needed. Do NOT use inline XML strings.
   ```cmake
   include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
   pj_embed_ui(my_plugin
     UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/dialog.ui
     HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dialog_ui.hpp
     VAR_NAME kDialogUi
   )
   ```
3. QDialogButtonBox **must** have `name="buttonBox"` and `standardButtons` property set:
   ```xml
   <widget class="QDialogButtonBox" name="buttonBox">
     <property name="standardButtons">
       <set>QDialogButtonBox::Cancel|QDialogButtonBox::Ok</set>
     </property>
   </widget>
   ```
   The DialogEngine searches for `findChild<QDialogButtonBox*>("buttonBox")` to wire accept/reject signals.
4. DataSource owns dialog as member: `CsvDialog dialog_;`
5. Override `dialogContext()`: `return &dialog_;`
6. Export with `PJ_DIALOG_PLUGIN(CsvDialog)` alongside `PJ_DATA_SOURCE_PLUGIN`

### Host Dialog Flow (how pj_proto_app uses it)

```
File: pj_proto_app/src/main_window.cpp, lines 155-175
```

1. Host checks `capabilities & kCapabilityHasDialog`
2. **Loads config with filepath BEFORE showing dialog** — so the dialog can analyze the file:
   ```cpp
   temp_handle.loadConfig(R"({"filepath":"/path/to/file"})");
   ```
3. Gets `dialogContext()` → creates `DialogEngine` → calls `showDialog(parent)`
4. On accept: retrieves `savedConfig()` and passes to `loadConfig()` for import

**PITFALL (hit during porting):** If you don't load the filepath config before showing the dialog, the dialog appears empty (no columns, no preview) because it doesn't know which file to analyze.

### Host Config Persistence (QSettings)

```
File: pj_proto_app/src/main_window.cpp, onLoadFile()
```

The host persists the plugin's last-used config in QSettings so dialogs remember user choices across sessions:

1. **Before dialog:** Restore saved config, merge in new filepath:
   ```cpp
   auto saved = settings.value("PluginConfig/<plugin_name>").toString().toStdString();
   auto cfg = nlohmann::json::parse(saved);  // last session's choices
   cfg["filepath"] = new_filepath;            // override with current file
   temp_handle.loadConfig(cfg.dump());        // pre-populate dialog
   ```

2. **After successful import:** Save the config:
   ```cpp
   settings.setValue("PluginConfig/<plugin_name>", QString::fromStdString(config));
   ```

This preserves: delimiter choice, time column selection, custom format, etc.

### Dialog Widget Naming — Critical Constraints

- `QDialogButtonBox` **must** be named `"buttonBox"` (not `"button_box"`) — the DialogEngine searches by this exact name
- `QDialogButtonBox` **must** have `standardButtons` property set in the XML, or no OK/Cancel buttons appear
- `QTextEdit` is **not supported** by the widget binding system — use `QTableWidget` instead for previews
- `setSelectedItems` takes a vector of strings (not indices) — use the item text, not a numeric index

---

## 5. CSV Plugin — Specific Design Decisions

### Data Model: Single topic, columns as fields

- One topic per file (named after file basename without extension)
- Each CSV column becomes a named field in that topic
- The time column is excluded from data fields (used only for timestamps)

### Sparse CSV Handling

The turtle.csv dataset has sparse data: turtle1 columns are empty on rows where turtle2 has data. The correct approach:

1. Pre-register ALL columns before writing
2. Group data points by timestamp (merge across columns)
3. For each timestamp, write a record with only the columns that have data
4. `finishRow()` fills missing columns with null automatically

### Timestamp Column Selection

When user selects a time column via dialog:
- CSV parser uses that column's values as timestamps (detected type: EPOCH_SECONDS, DATETIME, etc.)
- Timestamps are converted to absolute nanoseconds: `int64_t(seconds * 1e9)`
- Time column itself is excluded from data fields

When no time column selected (row number mode):
- Row index is used as timestamp (0, 1, 2, ... in nanoseconds)

---

## 6. Testing with pj_proto_app

### CLI Options

```bash
./build/pj_proto_app/pj_proto_app \
  --plugin-dir ./build/pj_ported_plugins/bin/ \
  --load /path/to/file.csv \       # auto-load file (skips dialog)
  --plot 3 \                        # auto-plot first N fields
  --screenshot /tmp/output.png      # take screenshot and exit
```

### ASAN Testing

```bash
./build.sh --debug   # builds with ASAN in build/debug_asan/
./build/debug_asan/pj_proto_app/pj_proto_app \
  --plugin-dir ./build/debug_asan/pj_ported_plugins/bin/ \
  --load turtle.csv --plot 3 --screenshot /tmp/test.png
# Check stderr for ASAN errors
```

### Plugin Output Directory

All plugin `.so` files are built into a single directory for easy loading:
```
build/pj_ported_plugins/bin/   # set by CMAKE_LIBRARY_OUTPUT_DIRECTORY in umbrella CMakeLists.txt
```

This is set in `pj_ported_plugins/CMakeLists.txt`:
```cmake
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
```

### Reference Test Files

- `~/ws_plotjuggler/src/PlotJuggler/datasamples/turtle.csv` — sparse CSV with epoch timestamps, 563 rows, 13 columns. Time range ~20.6 seconds. Good test for sparse data handling.
- `/tmp/test_simple.csv` — create with: `echo -e "time,x,y\n0,1.0,2.0\n1,3.0,4.0\n2,5.0,6.0"` for quick smoke tests

### Full Validation Workflow

```bash
./build.sh                    # RelWithDebInfo
./build.sh --debug            # Debug + ASAN
./test.sh                     # all unit tests (both build dirs)

# Smoke test: load CSV, auto-plot, screenshot
./build/pj_proto_app/pj_proto_app \
  --plugin-dir ./build/pj_ported_plugins/bin/ \
  --load ~/ws_plotjuggler/src/PlotJuggler/datasamples/turtle.csv \
  --plot 3 --screenshot /tmp/test.png

# ASAN smoke test (catches heap overflows, use-after-free)
./build/debug_asan/pj_proto_app/pj_proto_app \
  --plugin-dir ./build/debug_asan/pj_ported_plugins/bin/ \
  --load ~/ws_plotjuggler/src/PlotJuggler/datasamples/turtle.csv \
  --plot 3 --screenshot /tmp/test_asan.png
# If ASAN reports errors, check stderr
```

---

## 7. CSV Plugin — Remaining Gaps (Audit Results)

The CSV plugin was audited feature-by-feature against the old plugin. **45 features ported, 6 partial, 12 missing.**

### Critical Missing Features

| Feature | Status | Description |
|---------|--------|-------------|
| **String series** | **FIXED** | String-only columns now written via `PrimitiveType::kString`. Both numeric and string columns are pre-registered and included in the merged timeline. |
| **Non-monotonic time warning** | **FIXED** | Warning reported via `reportMessage(kWarning, ...)`. Note: old plugin showed Continue/Abort QMessageBox — a true blocking prompt requires extending the runtime host protocol (see Dialog Protocol section). |
| **Skipped lines detail** | **FIXED** | Per-line detail now reported via `reportMessage(kWarning, "Line N: reason\n...")`. Old plugin showed a QMessageBox with detailed text. |
| **Double-click column to accept** | **SDK LIMITATION** | Dialog SDK has no `onItemDoubleClicked` event. Requires protocol extension. |
| **Column history** | NOT PORTED | Old persists last-50 time column selections via QSettings. Would need dialog-side QSettings or host-provided persistence. |
| **Date Format Help dialog** | NOT PORTED | Old has a separate dialog with format reference tables. Could be implemented as a second dialog or inline help text. |
| **Raw text preview tab** | NOT PORTED | Old uses QCodeEditor with CSV syntax highlighting. Dialog SDK doesn't support QTextEdit. Requires protocol extension. |

### Partial/Minor Gaps

| Feature | Description |
|---------|-------------|
| **Preview label** | Says "first 20 rows" but code reads 100. Cosmetic — fix the label. |
| **toDouble** | Old uses QLocale::c() after replacing all commas. New uses strtod with smarter single-comma logic. More correct but slightly different behavior for edge cases like `"1,000,000"`. |
| **Dialog geometry** | Old saves/restores dialog geometry. New relies on dialog SDK defaults. |
| **Splitter ratio** | Old sets explicit stretch factors (1:2). New uses default. |

### Confirmed Bugs in New Code

1. ~~**Windows path separator**~~: **FIXED** — now uses `find_last_of("/\\")` to handle both separators.
2. **Time column skipped by name not index**: If two columns have the same raw name (before dedup), the wrong one could be skipped. Unlikely but fragile — dedup ensures unique names.
3. ~~**Preview label says "20 rows"**~~: **FIXED** — now says "100 rows" matching the actual code.

---

## 8. Unverified Plugins — Known Risks

The following plugins compile and have unit tests but have **NOT been tested end-to-end** in pj_proto_app. They likely have similar issues to those found in the CSV plugin:

| Plugin | Risk | Likely Issue |
|--------|------|-------------|
| DataLoadParquet | Medium | May need pre-registration of columns; timestamp precision loss (double→int64) |
| DataLoadULog | Medium | Same pre-registration issue possible; binary format edge cases |
| DataLoadMCAP | Low | Uses delegated ingest (no direct column writes) |
| ParserProtobuf | Low | Unit tested; native types now preserved |
| ParserROS | Medium | No unit tests; rosx_introspection API may have changed |
| DataStreamZMQ | Medium | strtod on non-null-terminated buffer was fixed; only parses text doubles |
| DataStreamMQTT | Medium | Encoding hardcoded (now configurable); binding cache added |
| DataStreamFoxgloveBridge | High | parser_encoding was wrong (fixed: ch.encoding not ch.schema_encoding) |
| DataStreamPJBridge | Medium | ZSTD decompression untested with real data |

### Common Issues to Check in Each Plugin

1. **Pre-register columns** before writing any data (if using direct ingest)
2. **ValueRef types** — don't cast to double; push native int64/uint64/bool
3. **string_view lifetime** — ensure owned strings outlive appendRecord call
4. **Null handling** — readers must check `isNull()` before using values
5. **Non-null-terminated buffers** — copy to `std::string` before `strtod`/parsing
6. **Delegated ingest encoding** — `parser_encoding` must be the wire format (e.g. `"cdr"`), not the schema format (e.g. `"ros2msg"`)
