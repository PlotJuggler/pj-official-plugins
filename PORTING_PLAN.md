# Porting Gap Analysis

Comparison of the 12 ported plugins against their original reference
implementations in `${PJ3_SOURCE_DIR}/plotjuggler_plugins/`, where `PJ3_SOURCE_DIR`
is a checkout of [PlotJuggler 3](https://github.com/facontidavide/PlotJuggler).

Plugins with no PJ3 original (e.g. `data_load_3d`) are out of scope here.

Only open gaps are listed; sections without a table have none.

Severity levels:
- **CRITICAL** — breaks wire compatibility, data correctness, or removes a major user-visible workflow
- **HIGH** — removes a feature users rely on regularly
- **MEDIUM** — removes a convenience feature or changes behavior in a noticeable way
- **LOW** — cosmetic or architectural difference with minimal user impact

---

## 1. data_load_csv (vs DataLoadCSV)

No open gaps.

---

## 2. data_load_mcap (vs DataLoadMCAP)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Keyboard shortcuts (Ctrl+A, Ctrl+Shift+A) missing | MEDIUM | `dialog_mcap.cpp:20-21, 33-44` | Original creates QShortcut objects for select-all / deselect-all (visible rows only). Ported has no shortcuts. |
| 2 | Sort column persistence removed | LOW | `dialog_mcap.cpp:124-127` | Original restores sort order from QSettings on dialog reopen. |
| 3 | Row disabling for empty channels | MEDIUM | `dialog_mcap.cpp:112-116` | Original grays out and disables rows with `message_count=0`. Ported shows them as normal selectable rows. |
| 4 | Data Tamer schema detection removed | MEDIUM | `dataload_mcap.cpp:359-367` | Original detects `data_tamer_msgs/msg/Schemas` and `data_tamer_msgs/msg/Snapshot` channels for special handling. Ported removes this entirely. |
| 5 | Parser error aggregation removed | LOW | `dataload_mcap.cpp:409-426` | Original collects all parser errors and shows a single summary dialog with affected topics. Ported reports individually. |

---

## 3. data_load_parquet (vs DataLoadParquet)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Column history/prioritization removed | MEDIUM | `dataload_parquet.cpp:19-44, 266-274` | Original reorders columns based on QSettings history of previously-selected time columns. Ported always uses schema order. |
| 2 | Progress update granularity changed | LOW | `dataload_parquet.cpp:397` | Original updates every 10 columns; ported updates per-batch. |

---

## 4. data_load_ulog (vs DataLoadULog)

No open gaps.

---

## 5. data_stream_foxglove_bridge (vs DataStreamFoxgloveBridge)

No open gaps.

**Intentional additive enhancement (not a gap):** the port routes channels per-encoding via
`classifyChannel`: CDR/ros2msg+omgidl go to `parser_ros` (as upstream) **plus** `protobuf` goes to
`parser_protobuf` (base64-decoding the advertised `FileDescriptorSet`). The upstream original was
CDR/ros2msg-only. Documented in the plugin README.

---

## 6. data_stream_mqtt (vs DataStreamMQTT)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Max inflight messages config missing | LOW | `mqtt_client.cpp:174` | Original configures `mosquitto_max_inflight_messages_set()`. |
| 2 | Keepalive timeout hardcoded | LOW | `mqtt_client.cpp:174, 186` | Original uses configurable keepalive; ported hardcodes 5 seconds. |
| 3 | Bind address support missing | LOW | `mqtt_client.cpp:187` | Original supports `mosquitto_connect_bind_v5()`. |
| 4 | Failed parse notification badge missing | MEDIUM | `datastream_mqtt.cpp:44, 210-212` | Original tracks failed parse count and shows QAction notification. Ported uses reportMessage only. |

---

## 7. data_stream_pj_bridge (vs DataStreamPlotJugglerBridge)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Error dialogs replaced with silent logging | MEDIUM | `websocket_client.cpp:369-370, 386, 425-436` | Original shows QMessageBox for connection errors, server errors, disconnects. Ported silently logs. |
| 2 | Message counting/stats infrastructure removed | LOW | `websocket_client.cpp:85-90, 834-855` | Original tracks per-topic message counts and debug frame stats. Ported discards this. |
| 3 | Scroll position restoration missing | LOW | `websocket_dialog.cpp:133-138` | Original saves/restores scroll position during topic list refresh via QTimer::singleShot. |

---

## 8. data_stream_zmq (vs DataStreamZMQ)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Topic parsing whitespace handling differs | LOW | `datastream_zmq.cpp:357` vs `zmq_source.cpp:62-79` | Original uses regex `QRegExp("(,{0,1}\\s+)|(;\\s*)")` which strips whitespace. Ported character-by-character parser preserves spaces within topic names. Edge case but could cause mismatch. |

Timestamps are nanoseconds in the SDK (the original produced microseconds); this is intentional.

---

## 9. parser_json (vs nlohmann_parsers in plotjuggler_app)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Timestamp field name configuration UI missing | MEDIUM | `nlohmann_parsers.h:86-206` | Original has QCheckBox + QLineEdit for selecting timestamp field name. Ported has no configuration at all. |
| 2 | No dialog support | MEDIUM | (entire dialog section in original) | Original has parser-specific configuration widget. Ported has none. |

---

## 10. parser_protobuf (vs ParserProtobuf)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Proto file loader UI / include dir management missing | HIGH | `protobuf_factory.cpp:44-227`, `protobuf_parser.ui` | Original has full dialog for loading `.proto` files and managing include directories. Ported has no UI. |

---

## 11. parser_ros (vs ParserROS)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | No configuration dialog | MEDIUM | (original has parser-specific QSettings integration) | Original persists settings via QSettings. Ported uses JSON config but no interactive dialog. |

Native int64/uint64 types are preserved, so the original's truncation check has no equivalent.

---

## 12. parser_data_tamer (vs ParserDataTamer)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| — | No significant gaps | — | — | Minimal API surface change (`getSeries` → `appendRecord`). Functionally equivalent. |

---

## Cross-cutting gaps (affect multiple plugins)

| # | Pattern | Affected Plugins | Notes |
|---|---------|-----------------|-------|
| 1 | QSettings → JSON config migration | All plugins | Original uses QSettings for cross-session persistence. Ported uses JSON strings via SDK config. Not directly compatible; no migration path for existing users. |
| 2 | Error dialogs replaced with silent logging | Foxglove, MQTT, PJ Bridge | Original shows QMessageBox for errors. Ported logs via `reportMessage()`: better for headless, worse for interactive debugging. |
| 3 | Streaming topic list periodic refresh | Foxglove, MQTT, PJ Bridge, ZMQ | All streaming plugins with topic selection should refresh the topic list periodically (~1s), even if the original did not. |
