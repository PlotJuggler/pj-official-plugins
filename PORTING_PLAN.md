# Porting Gap Analysis

Systematic comparison of all 12 ported plugins against their original reference
implementations in `~/ws_plotjuggler/src/PlotJuggler/plotjuggler_plugins/`.

Severity levels:
- **CRITICAL** — breaks wire compatibility, data correctness, or removes a major user-visible workflow
- **HIGH** — removes a feature users rely on regularly
- **MEDIUM** — removes a convenience feature or changes behavior in a noticeable way
- **LOW** — cosmetic or architectural difference with minimal user impact

---

## 1. data_load_csv (vs DataLoadCSV)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | DateTimeHelp dialog completely missing | HIGH | `datetimehelp.h/cpp/ui` (entire file set) | Original has a dedicated dialog showing date/time format code reference tables. Button exists in ported `.ui` but is orphaned — clicking it does nothing. Requires SDK sub-dialog support. |
| 2 | Column selection history not preserved | MEDIUM | `dataload_csv.cpp:32-62` (`prioritizedColumns()`, `updateColumnHistory()`) | Original tracks previously-selected time columns via QSettings and reorders the column list to show recent choices first. Ported has no history. |
| 3 | "Date Format Help" button orphaned | HIGH | `dataload_csv.ui:168-178` | Button widget exists in the `.ui` file but no event handler is wired. Consequence of gap #1. |

---

## 2. data_load_mcap (vs DataLoadMCAP)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Keyboard shortcuts (Ctrl+A, Ctrl+Shift+A) missing | MEDIUM | `dialog_mcap.cpp:20-21, 33-44` | Original creates QShortcut objects for select-all / deselect-all (visible rows only). Ported has no shortcuts. |
| 2 | Sort column persistence removed | LOW | `dialog_mcap.cpp:124-127` | Original restores sort order from QSettings on dialog reopen. |
| 3 | Row disabling for empty channels | MEDIUM | `dialog_mcap.cpp:112-116` | Original grays out and disables rows with `message_count=0`. Ported shows them as normal selectable rows. |
| 4 | Data Tamer schema detection removed | MEDIUM | `dataload_mcap.cpp:359-367` | Original detects `data_tamer_msgs/msg/Schemas` and `data_tamer_msgs/msg/Snapshot` channels for special handling. Ported removes this entirely. |
| 5 | Select/Deselect buttons coded but no UI widgets | MEDIUM | `mcap_dialog.hpp:129-144` | Code references `btnSelectAll`/`btnDeselectAll` that don't exist in `dialog_mcap.ui`. Dead code — buttons never appear. |
| 6 | Parser error aggregation removed | LOW | `dataload_mcap.cpp:409-426` | Original collects all parser errors and shows a single summary dialog with affected topics. Ported reports individually. |

---

## 3. data_load_parquet (vs DataLoadParquet)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Column history/prioritization removed | MEDIUM | `dataload_parquet.cpp:19-44, 266-274` | Original reorders columns based on QSettings history of previously-selected time columns. Ported always uses schema order. |
| 2 | Row sorting by timestamp within batch removed | HIGH | `dataload_parquet.cpp:372-373` | Original sorts rows by timestamp value to ensure ascending order per batch. Ported processes in raw batch order. May produce non-monotonic timestamps. |
| 3 | Timezone offset handling removed | HIGH | `dataload_parquet.cpp:176-185` | Original applies timezone offset for non-UTC Arrow TIMESTAMP columns via QTimeZone. Ported explicitly documents this as unsupported (line 100-102). |
| 4 | Progress update granularity changed | LOW | `dataload_parquet.cpp:397` | Original updates every 10 columns; ported updates per-batch. |

---

## 4. data_load_ulog (vs DataLoadULog)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Parameters dialog UI missing | HIGH | `ulog_parameters_dialog.h/cpp/ui` (entire file set) | Original shows an interactive 3-tab dialog (Info, Properties, Message Logs) after loading. Ported silently ingests parameters as `_parameters/` topic — no display. **BLOCKED: requires SDK "plugin status panel" design (see cross-cutting #7).** |
| 2 | Info metadata fields not displayed | HIGH | `ulog_parser.cpp:670-760`, `ulog_parameters_dialog.cpp:16-23` | Original parses and displays file info fields (hardware, software version, etc.) in a table. Ported does not extract these. **BLOCKED: same as #1.** |
| 3 | Embedded log messages not extracted | HIGH | `ulog_parser.cpp:84-93`, `ulog_parameters_dialog.cpp:39-77` | Original extracts LOGGING messages with level translation (EMERGENCY→DEBUG) and displays them. Ported ignores these. **BLOCKED: same as #1.** |
| 4 | Timestamp field handling changed | MEDIUM | `ulog_parser.cpp:165-171, 645-648` | Original detects timestamp field from format definition. Ported assumes timestamp at fixed message offset (first 8 bytes). Works for standard messages but fragile. |

---

## 5. data_stream_foxglove_bridge (vs DataStreamFoxgloveBridge)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Pause/resume functionality missing | HIGH | `foxglove_client.cpp:275-301`, `foxglove_client.h:57, 96-97` | Original has a Pause action that toggles `_paused` flag and stops processing incoming messages. Ported has no equivalent. |
| 2 | Unadvertise handling missing | HIGH | `foxglove_client.cpp:599-658` | Original handles channel removal from server with user-facing warning dialog and subscription cleanup. Ported ignores unadvertise messages. |
| 3 | Server status/warning messages not handled | MEDIUM | `foxglove_client.cpp:660-691` | Original processes Foxglove "status" op and maps levels to user notifications. Ported ignores these. |
| 4 | Socket error dialogs removed | MEDIUM | `foxglove_client.cpp:526-538` | Original shows QMessageBox on socket errors. Ported fails silently. |
| 5 | Parser creation error collection removed | MEDIUM | `foxglove_client.cpp:357-375, 409-426` | Original collects all parser creation failures and shows a summary. Ported reports first failure only. |
| 6 | Reconnection strategy removed | MEDIUM | `foxglove_client.cpp:495-524` | Original has `onDisconnected()` error handling and recovery path. Ported has minimal disconnect handling. |

---

## 6. data_stream_mqtt (vs DataStreamMQTT)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | TLS certificate management UI missing | CRITICAL | `mqtt_client.cpp:157-172`, `mqtt_dialog.cpp:242-297` | Original has 3 file browsers for server CA, client cert, and private key. Ported only has a boolean SSL checkbox — cannot load certificates. |
| 2 | Live topic discovery missing | HIGH | `mqtt_dialog.cpp:73-84, 194-226` | Original connects to broker, polls `getTopicList()` every 2s, and populates QListWidget. Ported has a single topic filter string field. |
| 3 | Multi-topic selection missing | HIGH | `mqtt_dialog.cpp:101-110` | Original uses QListWidget for selecting multiple discovered topics. Ported subscribes to a single filter string (e.g. `#`). |
| 4 | MQTT protocol version control missing | MEDIUM | `mqtt_client.cpp:134` | Original supports versions 3.1, 3.1.1, 5.0 via `mosquitto_int_option()`. Ported uses library default. |
| 5 | Connect/Disconnect button missing | MEDIUM | `mqtt_dialog.cpp:155-192` | Original has a toggle button that triggers interactive connection. Ported dialog is passive. |
| 6 | Username/password authentication reduced | HIGH | `mqtt_client.cpp:145` | Original explicitly calls `mosquitto_username_pw_set()`. Ported code has no visible auth handling. |
| 7 | Reconnection detection missing | MEDIUM | `mqtt_client.cpp:39-47` | Original emits `disconnected()` signal and notifies user. Ported has no disconnect callback. |
| 8 | Max inflight messages config missing | LOW | `mqtt_client.cpp:174` | Original configures `mosquitto_max_inflight_messages_set()`. |
| 9 | Keepalive timeout hardcoded | LOW | `mqtt_client.cpp:174, 186` | Original uses configurable keepalive; ported hardcodes 5 seconds. |
| 10 | Bind address support missing | LOW | `mqtt_client.cpp:187` | Original supports `mosquitto_connect_bind_v5()`. |
| 11 | Failed parse notification badge missing | MEDIUM | `datastream_mqtt.cpp:44, 210-212` | Original tracks failed parse count and shows QAction notification. Ported uses reportMessage only. |

---

## 7. data_stream_pj_bridge (vs DataStreamPlotJugglerBridge)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | **Binary wire format incompatible (u16 vs u32 topic_len)** | **CRITICAL** | `websocket_client.cpp:561` vs `pj_bridge_protocol.cpp:49-50` | Original reads topic name length as `u16`. Ported reads it as `u32`. **This breaks interoperability with original PJ servers.** |
| 2 | Pause/resume commands missing | HIGH | `websocket_client.cpp:275-301` | Original sends `pause`/`resume` JSON commands to server. Ported has no equivalent. |
| 3 | Topic filtering is a TODO stub | HIGH | `pj_bridge_dialog.hpp:319-327` | Code comment explicitly says "TODO" — filtering is non-functional. Original hides/shows QTreeWidget items with case-insensitive match. |
| 4 | Heartbeat keep-alive missing | HIGH | `websocket_client.cpp:31-36, 733-745` | Original sends periodic heartbeat via 1s timer. Ported sends none — WebSocket may timeout on idle connections. |
| 5 | Error dialogs replaced with silent logging | MEDIUM | `websocket_client.cpp:369-370, 386, 425-436` | Original shows QMessageBox for connection errors, server errors, disconnects. Ported silently logs. |
| 6 | Message counting/stats infrastructure removed | LOW | `websocket_client.cpp:85-90, 834-855` | Original tracks per-topic message counts and debug frame stats. Ported discards this. |
| 7 | Scroll position restoration missing | LOW | `websocket_dialog.cpp:133-138` | Original saves/restores scroll position during topic list refresh via QTimer::singleShot. |
| 8 | Flags field validation missing | LOW | `websocket_client.cpp:637-640` | Original validates `flags == 0` in binary header. Ported skips this check. |

---

## 8. data_stream_zmq (vs DataStreamZMQ)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Topic parsing whitespace handling differs | LOW | `datastream_zmq.cpp:357` vs `zmq_source.cpp:62-79` | Original uses regex `QRegExp("(,{0,1}\\s+)|(;\\s*)")` which strips whitespace. Ported character-by-character parser preserves spaces within topic names. Edge case but could cause mismatch. |
| 2 | Timestamp precision changed (microseconds → nanoseconds) | LOW | `datastream_zmq.cpp:259-261` vs `zmq_source.cpp:119` | Original produces microseconds (`1e-6 * count`); ported produces nanoseconds (`* 1e9`). Both are internally consistent with their respective SDKs. Not a bug. |

---

## 9. parser_json (vs nlohmann_parsers in plotjuggler_app)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | CBOR, BSON, MessagePack formats removed | HIGH | `nlohmann_parsers.h:36-82` | Original supports 4 formats via nlohmann library. Ported only handles JSON. Any source sending CBOR/BSON/MessagePack will fail. |
| 2 | Timestamp field name configuration UI missing | MEDIUM | `nlohmann_parsers.h:86-206` | Original has QCheckBox + QLineEdit for selecting timestamp field name. Ported has no configuration at all. |
| 3 | No dialog support | MEDIUM | (entire dialog section in original) | Original has parser-specific configuration widget. Ported has none. |

---

## 10. parser_protobuf (vs ParserProtobuf)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Embedded timestamp extraction missing | CRITICAL | `protobuf_parser.cpp:47-70` | Original auto-detects a "timestamp" field (double type) and uses it as message time. Ported ignores this entirely. Messages lose embedded timing. |
| 2 | Enum values stored as int instead of string | HIGH | `protobuf_parser.cpp:178-185` vs `protobuf_parser.cpp:104-109` (ported) | Original stores enum names as string series (human-readable). Ported converts to int32 only. |
| 3 | String field handling changed | MEDIUM | `protobuf_parser.cpp:187-199` vs `protobuf_parser.cpp:153` (ported) | Original includes string fields < 100 bytes as string series. Ported skips ALL string fields. |
| 4 | Array size policy (clamp/discard) missing | MEDIUM | `protobuf_parser.cpp:114-124` | Original supports `maxArraySize()` / `clampLargeArray()` configuration. Ported has no array limiting. |
| 5 | Proto file loader UI / include dir management missing | HIGH | `protobuf_factory.cpp:44-227`, `protobuf_parser.ui` | Original has full dialog for loading `.proto` files and managing include directories. Ported has no UI. |

---

## 11. parser_ros (vs ParserROS)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| 1 | Truncation check config removed | MEDIUM | `ros_parser.h:76`, `ros_parser.cpp:148-175` | Original has `enableTruncationCheck()` for int64→double safety. Ported removes this — silent precision loss for large int64 values. |
| 2 | Array policy hardcoded to KEEP | MEDIUM | `ros_parser.cpp:180-187` vs `ros_parser.cpp:184` (ported) | Original allows KEEP vs DISCARD large arrays. Ported always keeps — unbounded memory consumption for large repeated fields. |
| 3 | No configuration dialog | MEDIUM | (original has parser-specific QSettings integration) | Original persists settings via QSettings. Ported uses JSON config but no interactive dialog. |

---

## 12. parser_data_tamer (vs ParserDataTamer)

| # | Gap | Severity | Original Location | Notes |
|---|-----|----------|-------------------|-------|
| — | No significant gaps | — | — | Minimal API surface change (`getSeries` → `appendRecord`). Functionally equivalent. |

---

## Cross-cutting gaps (affect multiple plugins)

| # | Pattern | Affected Plugins | Notes |
|---|---------|-----------------|-------|
| 1 | Dialog geometry persistence must be host-managed | All plugins with dialogs | The host should automatically persist and restore dialog geometry per plugin type. Individual plugins should not need to implement this. **SDK/host change required.** |
| 2 | QSettings → JSON config migration | All plugins | Original uses QSettings for cross-session persistence. Ported uses JSON strings via SDK config. Not directly compatible — no migration path for existing users. |
| 3 | Error dialogs replaced with silent logging | Foxglove, MQTT, PJ Bridge | Original shows QMessageBox for errors. Ported logs via `reportMessage()`. Better for headless, worse for interactive debugging. |
| 4 | Pause/resume functionality removed | Foxglove, PJ Bridge | Original has pause action in toolbar. Resolved by cross-cutting #11 (streamer lifecycle controls). |
| 5 | Column history / selection memory removed | CSV, Parquet | Original tracks frequently-selected columns. Ported always shows columns in schema order. |
| 6 | Warning/error deduplication API needed | MCAP, Foxglove, others | Multiple plugins independently need to deduplicate repeated warnings (e.g. same encoding error for many topics). Design a holistic `reportMessage()` deduplication mechanism rather than ad-hoc per-plugin sets. **SDK/host change required.** |
| 7 | Plugin status panel for post-load info | ULog (parameters, info, logs), potentially others | ULog originally shows a 3-tab dialog with file metadata, parameters, and embedded logs. A general-purpose "plugin status/info panel" in the host would serve ULog and any future plugin that needs to present non-dialog information. **SDK/host design discussion required.** |
| 8 | Streaming topic list periodic refresh | Foxglove, MQTT, PJ Bridge, ZMQ | All streaming plugins with topic selection should refresh the topic list periodically (~1s). Apply consistently to all plugins, even if the original did not implement this. |
| 9 | Add README.md per plugin | All 12 plugins | Each plugin subfolder should contain a README.md with a short introduction and any user-facing information (supported formats, configuration, known limitations). |
| 10 | Sub-dialog support in dialog SDK | CSV (DateTimeHelp), potentially others | The dialog SDK currently cannot launch a secondary dialog from a button click. Required for porting CSV DateTimeHelp and potentially other features. **SDK protocol extension required.** |
| 11 | Streamer lifecycle controls in proto_app | All streaming plugins | Right-click context menu on dataset nodes in the sidebar tree with **Pause / Resume / Stop / Restart** actions. Visual feedback (icon or decoration) showing current state (running / paused / stopped). The SDK protocol already has `pause()`/`resume()` vtable slots, `SUPPORTS_PAUSE` capability flag, and `running ⇌ paused` state transition — but none of this is wired on the host side. **Required work:** (a) expose `pause()`/`resume()` through `DataSourceHandle` and `DataSourceSession`, (b) add context menu to `SeriesTreeModel` dataset nodes, (c) add state icons/decoration, (d) streaming plugins that want pause should declare `kCapabilitySupportsPause` and override `pause()`/`resume()`. **Proto_app + SDK host change required.** |

---

## Statistics

| Severity | Count |
|----------|-------|
| CRITICAL | 3 |
| HIGH | 16 |
| MEDIUM | 17 |
| LOW | 10 |
| **Total per-plugin gaps** | **46** |
| **Cross-cutting items** | **11** |

### CRITICAL items — ALL FIXED:
1. ~~**MQTT TLS certificates**~~ — DONE: certificate file pickers + SSL wiring
2. ~~**Protobuf embedded timestamp**~~ — DONE: auto-detects "timestamp" double field
3. ~~**PJ Bridge wire format**~~ — DONE: u32 → u16 topic_len restored

### Per-plugin fixes completed:
- CSV #2: Column selection history (auto-selects from prior sessions)
- MCAP #5: Select All / Deselect All buttons added to .ui
- MQTT #1: TLS certificate management UI (load/erase/persist)
- MQTT #6: Username/password authentication wired
- Parquet #2: Row sorting by timestamp within batch
- Parquet #3: Timezone offset handling for fixed-offset zones
- Protobuf #1: Embedded timestamp extraction
- Protobuf #2: Enum values stored as strings (human-readable)
- Protobuf #3: String fields included (< 100 bytes)
- Protobuf #4: Array size policy (clamp/discard via config)
- ROS #2: Array policy configurable (KEEP/DISCARD via config)
- JSON #1: CBOR, BSON, MessagePack support added
- Foxglove #2: Unadvertise handling with subscription cleanup
- Foxglove #3: Server status/warning message handling
- Foxglove #4: Disconnect detection with reportMessage
- Foxglove #5: Parser error collection and aggregated reporting
- PJ Bridge #3: Topic filtering implemented (case-insensitive on name + type)
- PJ Bridge #4: Heartbeat keep-alive (~1s interval)
- PJ Bridge #8: Flags field validation (16-byte header)

### Cross-cutting features completed:
- #9: README.md added for all 12 plugins
- #11: Streamer lifecycle controls (right-click: Pause/Resume/Stop/Restart/Remove + state icons)

### SDK/host extensions completed (zero ABI changes):
- **Dialog geometry persistence** — DialogEngine auto-saves/restores via QSettings keyed by plugin name
- **Warning deduplication** — RuntimeHostState deduplicates by message text, prints only on first occurrence
- **Sub-dialog support** — WidgetData.requestSubDialog(ui_xml) + nested modal in DialogEngine
- **CSV DateTimeHelp** — ported using sub-dialog SDK (format reference tables)
- **ULog info/logs** — writes `_info/` and `_log/` topics from ulog_cpp metadata

### Gaps resolved by design (no fix needed):
- ROS #1: Truncation check — native int64/uint64 types preserved, no double conversion
- ULog #4: Timestamp field — first 8 bytes is correct per ULog spec
- ZMQ #2: Timestamp precision — nanoseconds is correct for new SDK

### Final round fixes:
- MQTT #2-3: Live topic discovery via dialog Connect button + multi-topic selection in QListWidget
- MQTT #4: Protocol version control (3.1, 3.1.1, 5.0 via comboBoxVersion)
- MQTT #5: Connect/disconnect button wired in dialog
- MQTT #7: Reconnection detection via connection_lost_handler callback
- Foxglove #6: Reconnection strategy (~5s retry loop in onPoll when disconnected)
- PJ Bridge #2: Pause/resume protocol commands to server (kCapabilitySupportsPause declared)

### Remaining SDK work (cosmetic, not blocking):
- **Plugin status panel UI** — proto_app could add a dock/tab to display `_info/`, `_log/`, `_parameters/` topics (data is already written by ULog)
