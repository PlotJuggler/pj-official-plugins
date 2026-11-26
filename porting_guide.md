# Plugin Porting Guide

Reference for porting PlotJuggler plugins to the new SDK. Only the CSV plugin is verified end-to-end; the other ported plugins (Parquet, ULog, MCAP, Protobuf, ROS, ZMQ, MQTT, Foxglove Bridge, PJ Bridge) are not, so expect similar issues (see section 8).

---

## 0. Porting Philosophy — Read This First

Porting is a **mechanical translation**, not a rewrite. The original plugin is the
specification: the port must behave identically in every observable way, using the new SDK as the
target language.

### What "translation" means in practice

1. **Read the original code in its entirety before writing anything**, line by line: every
   function, signal/slot connection and conditional branch.

2. **Every code path in the original must have a corresponding code path in the port.** 14 `if`
   branches in the original means 14 in the port; a warning on a specific condition is emitted on the
   same condition; saved dialog geometry stays saved.

3. **There are no minor features.** Dialog geometry persistence, splitter ratios, a help button that
   opens a sub-dialog, column selection history: every feature the user can observe or benefit
   from is mandatory. Classify features only as "ported" or "not yet ported".

4. **Do not optimize, simplify, or "improve" the original's behavior.** Verbose logic, redundant
   checks and roundabout approaches are translated faithfully; the original was tested in
   production. If you believe something is a bug, flag it explicitly; do not silently fix it.

5. **Do not substitute a different approach.** `QTreeWidget` is not replaced by `QTableWidget` because
   it is "close enough". If the SDK does not support what the original uses, that is a blocker (see
   below).

6. **Speed does not matter. Completeness does.** A port may take multiple sessions. Declaring a
   plugin "ported" with silently dropped features is not acceptable.

### When you cannot translate something

If the SDK lacks a capability the original uses (a widget type, an event type, a host callback),
there are exactly two options:

- **Option A: Extend the SDK** to support it, then use it.
- **Option B: Stop and ask the user.**

Never silently drop the feature, substitute a simpler alternative, or mark it a "minor gap". If
unsure whether the SDK can do something, ask.

### Verification before claiming "done"

Before declaring a port complete, produce a **feature audit table** with one row per feature of
the original plugin:

| # | Original Feature | Original Code Location | Port Status | New Code Location | Notes |
|---|-----------------|----------------------|-------------|-------------------|-------|
| 1 | (feature name)  | (file:line)          | DONE / BLOCKED / NOT YET | (file:line) | (if BLOCKED: what SDK gap) |

Every row must be filled. The port is done only when every row says DONE or BLOCKED (with an
explicit SDK limitation and a user decision on how to proceed).

### Common failure modes to guard against

These have repeatedly caused incomplete ports. If you catch yourself doing one, stop and correct:

- **"This is just cosmetic"**: cosmetic features are features. Port them.
- **"The SDK doesn't support X, so I'll use Y instead"**: stop and ask.
- **"I'll come back to this later"**: port it now or mark it BLOCKED with a specific SDK limitation.
- **"The original code is messy, let me clean it up"**: translate it as-is; cleanup is a separate
  task after the translation is verified.
- **"This edge case probably never happens"**: the original author handled it; so do you.
- **"I'm almost done, just these last few things"**: run the audit table. Any row not DONE or
  BLOCKED means you are not almost done.

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
PJ_DIALOG_PLUGIN(DialogClassName, kMyManifest)  // if plugin has a dialog (pass the embedded manifest)
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

**PITFALL:** casting everything to `double` loses precision for int64/uint64 values. Push native types directly:

```cpp
// WRONG — loses precision for large integers
out.push_back({name, static_cast<double>(value.get<int64_t>())});

// CORRECT — preserves full precision
out.push_back({name, value.get<int64_t>()});
```

### NamedFieldValue uses std::string — safe by default

```cpp
struct NamedFieldValue {
  std::string name;   // owned
  ValueRef value;
};
```

`name` is owned, so field names can be built by string concatenation without dangling references:

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

### 3a. ensureColumn() after rows are written

```
File: pj_datastore/src/writer.cpp
```

`ensureColumn()` seals the current chunk when a new column is added after rows already exist; the
expanded column set continues in a fresh chunk, and readers treat absent columns in earlier chunks
as null.

**Pre-registration is optional but recommended when the schema is known upfront**: it avoids
mid-stream chunk sealing and enables the faster bound-write path (`appendBoundRecord`).

```cpp
auto topic = writeHost().ensureTopic(topic_name);

// Optional: pre-register fields when the schema is known upfront
for (const auto& col_name : all_column_names) {
  writeHost().ensureField(*topic, col_name, PJ::PrimitiveType::kFloat64);
}

// Write data — fields auto-create on first non-null value if not pre-registered
for (...) {
  writeHost().appendRecord(*topic, ts, fields);
}
```

### 3b. finishRow() auto-fills unset columns with null

```
File: pj_datastore/src/chunk.cpp
```

When `appendRecord()` sets only some fields, `finishRow()` pads the unset columns with null (zero bytes, validity bit cleared); sparse records are supported.

Example: topic has columns [x, y, z]. Record sets only x:
- Column x: value written
- Column y: null (zero bytes, validity bit = 0)
- Column z: null (zero bytes, validity bit = 0)

All columns always have the same row count (invariant).

### 3c. readColumnAsDoubles() returns NaN for nulls

```
File: pj_datastore/src/chunk.cpp
```

`readColumnAsDoubles()` converts raw bytes to double and **returns `NaN` for null rows** (not 0.0), so consumers need no `isNull()` check: NaN propagates and chart panels do not plot it.

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

### 3d. Chunks can have different column counts

Chunks sealed before a column was added have fewer columns. Check bounds before `readColumnAsDoubles(col_index, ...)`:

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

### Dialog Porting Rule

The dialog is user-facing behavior and is ported as exactly as the data logic:

- **Copy the original `.ui` file exactly.** Modify only what cannot work (e.g. custom widget
  classes whose headers don't exist here); layout, sizing, spacing, naming, properties and tab
  order stay identical.
- **Use real `.ui` files** (editable in Qt Creator), embedded into a `constexpr char[]` header via
  CMake's `pj_embed_ui()`. No inline XML strings; plugins must NOT depend on Qt for UI loading.
- **Smart dialog plugins**: streaming-source dialogs with dynamic topic discovery (PJ Bridge,
  Foxglove, MQTT) own the connection logic inside the dialog plugin, replicating the original
  Connect, discover, select, subscribe flow.
- **Every signal/slot connection in the original dialog must be replicated.** If the original wires
  `itemDoubleClicked` to `accept()`, so must the port; if the SDK lacks the event type, that is a
  blocker (extend the SDK or ask).

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

### When the original uses an unsupported widget

A widget type not in the table above (QTreeWidget, QTextEdit, QTableView, custom widgets) must
**not** be silently replaced by a supported one. Check whether the dialog protocol can be extended
(below); if so, extend it; if unsure, ask the user.

### Extending the Dialog Protocol

Extend `pj_plugins/dialog_protocol/` when a ported plugin needs an interaction the SDK does not
cover, but first **verify the existing interface truly cannot do it**; a workaround with supported
widgets is sometimes enough.

Already available (do not re-add):
- **Double-click on list item**: override `onItemDoubleClicked(widget_name, index)` (see `data_load_csv/csv_dialog.cpp`).
- **Sub-dialogs**: `WidgetData::requestSubDialog(ui_xml)`.
- **Blocking Continue/Abort prompt**: `runtimeHost().askContinue(...)`; `reportMessage` is fire-and-forget.

Known gaps:
- **QTextEdit / rich text**: no binding in `widget_binding.cpp` (follow the QLineEdit pattern).
- **Raw text preview with syntax highlighting**: needs QTextEdit support or a custom widget type.

Files to modify when extending:
- `pj_plugins/dialog_protocol/include/pj_plugins/sdk/widget_event.hpp`: new event types
- `pj_plugins/dialog_protocol/src/widget_binding.cpp`: widget type handling
- `pj_plugins/dialog_protocol/include/pj_plugins/sdk/widget_data.hpp`: new set* methods
- `pj_base/include/pj_base/data_source_protocol.h`: new runtime host callbacks

### Dialog Implementation Checklist

1. Subclass `PJ::DialogPluginTyped`
2. Use a real `.ui` file in the plugin's `ui/` directory. `pj_embed_ui()` generates a header;
   `ui_content()` returns the generated `constexpr char[]`.
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
6. Export with `PJ_DIALOG_PLUGIN(CsvDialog, kCsvManifest)` alongside `PJ_DATA_SOURCE_PLUGIN`

### Host Dialog Flow (how pj_proto_app uses it)

```
File: pj_proto_app/src/main_window.cpp
```

1. Host checks `capabilities & kCapabilityHasDialog`
2. **Loads config with filepath BEFORE showing dialog**, so the dialog can analyze the file:
   ```cpp
   temp_handle.loadConfig(R"({"filepath":"/path/to/file"})");
   ```
3. Gets `dialogContext()` → creates `DialogEngine` → calls `showDialog(parent)`
4. On accept: retrieves `savedConfig()` and passes to `loadConfig()` for import

**PITFALL:** without the filepath config loaded first, the dialog appears empty (no columns, no preview) because it does not know which file to analyze.

### Host Config Persistence (QSettings)

```
File: pj_proto_app/src/main_window.cpp, onLoadFile()
```

The host persists the plugin's last-used config in QSettings (delimiter, time column, custom format, ...):

1. **Before dialog:** restore the saved config and merge in the new filepath:
   ```cpp
   auto saved = settings.value("PluginConfig/<plugin_name>").toString().toStdString();
   auto cfg = nlohmann::json::parse(saved);  // last session's choices
   cfg["filepath"] = new_filepath;            // override with current file
   temp_handle.loadConfig(cfg.dump());        // pre-populate dialog
   ```

2. **After successful import:** save the config:
   ```cpp
   settings.setValue("PluginConfig/<plugin_name>", QString::fromStdString(config));
   ```

### Dialog Widget Naming: Critical Constraints

- `QDialogButtonBox` **must** be named `"buttonBox"` (not `"button_box"`); the DialogEngine searches by this exact name
- `QDialogButtonBox` **must** have `standardButtons` set in the XML, or no OK/Cancel buttons appear
- `QTextEdit` is **not supported** by the widget binding system (see "When the original uses an unsupported widget")
- `setSelectedItems` takes item text, not numeric indices

---

## 5. CSV Plugin — Specific Design Decisions

### Data Model: Single topic, columns as fields

- One topic per file (named after file basename without extension)
- Each CSV column becomes a named field in that topic
- The time column is excluded from data fields (used only for timestamps)

### Sparse CSV Handling

In turtle.csv, turtle1 columns are empty on rows where turtle2 has data. Handle sparse data by:

1. Pre-registering columns (optional but recommended; CSV headers are known upfront)
2. Grouping data points by timestamp (merge across columns)
3. Writing, per timestamp, a record with only the columns that have data; missing columns are auto-filled with null

### Timestamp Column Selection

With a time column selected in the dialog, its values are the timestamps (detected type: EPOCH_SECONDS, DATETIME, ...), converted to absolute nanoseconds (`int64_t(seconds * 1e9)`), and the column is excluded from data fields.

With no time column (row number mode), the row index is the timestamp (0, 1, 2, ... in nanoseconds).

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
  --load data_load_csv/test_data/turtle.csv --plot 3 --screenshot "$(mktemp -d)/test.png"
# Check stderr for ASAN errors
```

### Plugin Output Directory

All plugin `.so` files build into `build/pj_ported_plugins/bin/`, set by
`CMAKE_LIBRARY_OUTPUT_DIRECTORY` in `pj_ported_plugins/CMakeLists.txt`:
```cmake
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
```

### Reference Test Files

- `data_load_csv/test_data/turtle.csv` (tracked): sparse CSV with epoch timestamps, 563 rows, 13 columns, ~20.6 s; tests sparse data handling.
- A throwaway CSV in `$(mktemp -d)` for quick smoke tests: `echo -e "time,x,y\n0,1.0,2.0\n1,3.0,4.0\n2,5.0,6.0"`

### Full Validation Workflow

```bash
./build.sh                    # RelWithDebInfo
./build.sh --debug            # Debug + ASAN
ctest --test-dir build --output-on-failure              # unit tests
ctest --test-dir build/debug_asan --output-on-failure   # same, under ASAN

# Smoke test: load CSV, auto-plot, screenshot
./build/pj_proto_app/pj_proto_app \
  --plugin-dir ./build/pj_ported_plugins/bin/ \
  --load data_load_csv/test_data/turtle.csv \
  --plot 3 --screenshot "$(mktemp -d)/test.png"

# ASAN smoke test (catches heap overflows, use-after-free)
./build/debug_asan/pj_proto_app/pj_proto_app \
  --plugin-dir ./build/debug_asan/pj_ported_plugins/bin/ \
  --load data_load_csv/test_data/turtle.csv \
  --plot 3 --screenshot "$(mktemp -d)/test_asan.png"
# If ASAN reports errors, check stderr
```

---

## 7. CSV Plugin: Parity Notes

- **toDouble locale handling:** the original uses QLocale::c() after replacing all commas; the port
  uses strtod with different single-comma logic. It must match the original exactly, including
  inputs like `"1,000,000"`.
- **Time column skipped by name, not index:** if two columns share a raw name (before dedup) the
  wrong one could be skipped; dedup makes names unique, so this is fragile rather than broken.

---

## 8. Unverified Plugins: Known Risks

These plugins compile and have unit tests but have **not** been tested end-to-end in pj_proto_app:

| Plugin | Risk | Likely Issue |
|--------|------|-------------|
| DataLoadParquet | Medium | May need pre-registration of columns; timestamp precision loss (double to int64) |
| DataLoadULog | Medium | Same pre-registration issue possible; binary format edge cases |
| DataLoadMCAP | Low | Delegated ingest (no direct column writes) |
| ParserProtobuf | Low | Unit tested |
| ParserROS | Medium | rosx_introspection API changes |
| DataStreamZMQ | Medium | Only parses text doubles |
| DataStreamMQTT | Medium | Encoding must be configurable; binding cache |
| DataStreamFoxgloveBridge | High | `parser_encoding` must come from `ch.encoding`, not `ch.schema_encoding` |
| DataStreamPJBridge | Medium | ZSTD decompression untested with real data |

### Common Issues to Check in Each Plugin

1. **Pre-register columns** when the schema is known upfront (recommended for direct ingest, optional otherwise)
2. **ValueRef types**: don't cast to double; push native int64/uint64/bool
3. **string_view lifetime**: owned strings must outlive the `appendRecord` call
4. **Null handling**: readers must check `isNull()` before using values
5. **Non-null-terminated buffers**: copy to `std::string` before `strtod`/parsing
6. **Delegated ingest encoding**: `parser_encoding` must be the wire format (e.g. `"cdr"`), not the schema format (e.g. `"ros2msg"`)
