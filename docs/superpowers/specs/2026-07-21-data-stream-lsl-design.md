# Design: `data_stream_lsl` — Lab Streaming Layer streaming source

**Date:** 2026-07-21
**Status:** Approved design, ready for implementation plan
**Original:** https://github.com/PlotJuggler/plotjuggler-lsl (PJ3 plugin, MIT)
**New dependency:** Conan `liblsl/1.16.2` (CMake `find_package(LSL)`, target `LSL::lsl`)

## 1. Goal

Add a new streaming DataSource plugin, `data_stream_lsl`, that ingests
[Lab Streaming Layer](https://labstreaminglayer.org/) streams into PlotJuggler 4.
This is a **thoughtful re-integration**, not a mechanical port: the original PJ3
plugin's *user-facing features* are preserved, but the implementation is rebuilt
on the new SDK's idioms and two real limitations of the original are fixed.

## 2. Why this is not a mechanical port

The original was written for `PJ::DataStreamer` + Qt threads + `dataMap()`. The
new SDK inverts several of those choices, and LSL's data model makes some of the
original machinery unnecessary:

| Original (PJ3) | This design (PJ4 SDK) | Rationale |
|---|---|---|
| `PJ::DataStreamer`, writes into `dataMap()` | `PJ::StreamSourceBase`, **direct ingest** via `writeHost().appendBoundRecord()` | LSL delivers already-typed numeric/string channels; it is a `kCapabilityDirectIngest` source like `data_stream_dummy`, **not** a byte-parser source like UDP/ZMQ/MQTT. No message-parser, no encoding combo. |
| One `QThread` + `Streamer` per stream, `pull_chunk` + `msleep(50)` loop, queued signals | **No plugin-owned threads**; `onPoll()` calls `pull_chunk(timeout=0)` per inlet | An `lsl::stream_inlet` already runs its own internal receiver thread and buffers samples. The host's poll loop replaces the manual `msleep(50)` loop. |
| `StreamLSLDialog` `QDialog` + 1 s `QTimer` refresh | `LslDialog : DialogPluginTyped`, refresh driven by `onTick()` | The PJ4 dialog framework provides `onTick()` as the periodic hook (same pattern MQTT uses for live topic discovery). `.ui` is compiled to a header via `pj_embed_ui`. |
| Numeric (double) channels only; string channels silently dropped | Numeric channels kept in **native types**; **string channels supported** as `kString` series | LSL "Markers" streams (string channels carrying event triggers/annotations) are a primary LSL use case the original missed. `PrimitiveType::kString` is first-class in the write API. |
| Raw LSL sample timestamps forwarded as-is | **User-selectable timestamp source** (3 modes), default = time-corrected sync | LSL sample stamps are in the *sender's* `lsl::local_clock()` domain (monotonic since boot, not Unix epoch). PlotJuggler wants absolute epoch-ns. `inlet.time_correction()` was never called by the original. |
| `liblsl` vendored as a git submodule | Conan `liblsl/1.16.2`, static | Matches the repo's Conan dependency model and the "self-contained plugin artifact" rule (static link, hidden visibility, `RTLD_LOCAL`). |

## 3. Scope (this first version)

**In scope**
- Discovery dialog with live refresh, multi-select, Select All.
- Multiple simultaneous streams.
- Numeric channels in native types (`float32/64`, `int8/16/32/64`).
- String channels → `kString` series (marker/event streams).
- Three user-selectable timestamp modes.
- Conan `liblsl` integration + full repo wiring (root CMake, aggregate conanfile,
  README tables, CI matrix).
- Unit tests for the pure logic; sample publisher script.

**Out of scope (minimal first cut — deliberately deferred)**
- Reconnection / recovery when a selected stream is offline at start or drops
  mid-session. Inlets are opened once in `onStart()`; a stream that cannot be
  resolved is skipped with a reported message.
- Periodic `time_correction()` re-refresh during long sessions (captured once at
  inlet open).
- Per-channel units/metadata beyond the channel label.

These are noted as future work in §12 so the boundary is explicit.

## 4. Architecture

`LslSource : PJ::StreamSourceBase`, capabilities
`kCapabilityContinuousStream | kCapabilityDirectIngest | kCapabilityHasDialog`.

**Key LSL fact that simplifies typing:** `channel_format` is a property of the
**whole stream**, not per channel — every channel in a stream shares one format.
So each topic's fields are uniformly one `PrimitiveType`; there are no mixed-type
topics. A marker stream is simply a topic whose field(s) are `kString`.

### 4.1 File layout (mirrors `data_stream_udp`)

```
data_stream_lsl/
  CMakeLists.txt
  conanfile.py                 # plotjuggler_sdk, liblsl/1.16.2, nlohmann_json, gtest
  manifest.json                # id, name, family=data_source, abi
  datastream_lsl.ui            # discovery table + timestamp radio group + Select All
  lsl_dialog.hpp               # LslDialog : DialogPluginTyped
  lsl_source.cpp               # LslSource : StreamSourceBase + free helper functions
  lsl_conversions.hpp          # pure, testable helpers (see §8)
  tests/lsl_source_test.cpp    # unit tests for the pure helpers
  test_scripts/lsl_publisher.py  # sample publisher: one numeric + one marker stream
  README.md
```

`lsl_source.cpp` ends with `PJ_DATA_SOURCE_PLUGIN(LslSource, kLslManifest)` and
`PJ_DIALOG_PLUGIN(LslDialog)` (same dual-export as `data_stream_udp`).

## 5. Dialog (`LslDialog : DialogPluginTyped`)

### 5.1 Widgets (`datastream_lsl.ui`)
- **`tableStreams`** — multi-select table, columns **Name / Type / Channels / Rate / Source ID**.
  (Richer than PJ3's ID/Name/Type.) Rendered via `WidgetData::setTableRows` /
  list semantics; selection is **text-keyed** (per the sortable-selection contract,
  PR #188) so built-in column sort cannot desync the restored selection.
- **`buttonSelectAll`** — selects all currently listed rows.
- **`radioTimestampSync` / `radioTimestampRaw` / `radioTimestampReceiver`** — a
  button group choosing the timestamp source (same UX shape as the MCAP plugin's
  publishTime/logTime/embedded radios). Default = Sync.
- `QDialogButtonBox` named **`buttonBox`** (required for accept/reject wiring).

### 5.2 Discovery
- `resolveStreams()` calls `lsl::resolve_streams(timeout≈0.75)` on dialog open and
  again from `onTick()` (~1 s cadence). Diff against the last-seen set (keyed by
  `uid`); if unchanged, return `false` from `onTick()` (no re-render). Selection is
  preserved across refreshes because it is text-keyed to a stable identity, not row
  index.
- Discovery runs synchronously inside the dialog (LSL resolve is a short broadcast);
  no "Connect" button, unlike MQTT/Foxglove — LSL discovery is connectionless.

### 5.3 Config gating & persistence
- OK enabled iff ≥1 stream selected (`setOkEnabled`).
- `saveConfig()` / `loadConfig()` persist the **selected stream identities** and the
  timestamp mode (schema in §7). Identity is `source_id` when non-empty, else
  `name` + `type`. **`uid` is never persisted** (regenerated every session).

## 6. Source (`LslSource : StreamSourceBase`)

### 6.1 `onStart()`
1. Parse config: selected identities + timestamp mode.
2. For each identity:
   - `lsl::resolve_stream("source_id", id, 1, timeout)` (or name/type fallback).
     Not found within timeout → `reportMessage(kWarning, …)` and **skip** (minimal cut).
   - Construct `lsl::stream_inlet`.
   - Read `stream_info`: `channel_count()`, `channel_format()`, and channel labels
     from XML `desc().child("channels").child("channel")/label` (fallback
     `channel_<i>`).
   - `writeHost().ensureTopic(topic_name)` once (topic name per §6.4).
   - `writeHost().ensureField(topic, label, mapFormat(channel_format))` per channel;
     cache the `FieldHandle`s.
   - Capture timestamp constants **once**: `epoch_offset_ns = now_epoch_ns() −
     int64(lsl::local_clock() × 1e9)` and `time_corr_s = inlet.time_correction(timeout)`
     (guarded; on failure treat as 0).
   - Store an `LslInlet { inlet, topic, field_handles, format, channel_count,
     epoch_offset_ns, time_corr_s }`.
3. Return ok even if some streams were skipped, as long as ≥1 opened; if none
   opened, return `unexpected("no selected LSL stream could be resolved")`.

### 6.2 `onPoll()` (must not block)
For each `LslInlet`:
- Numeric formats: `pull_chunk<double>(chunk, stamps, 0.0)` (LSL converts native
  numeric → double; we then push in the field's native `PrimitiveType`, casting the
  double back to the native integer/float as appropriate). *Alternative considered:*
  template on the native C type; decided against to keep one code path — the plot
  values are identical and integer ranges used in practice fit in double exactly.
  (Revisit if a use case needs full int64 precision.)
- String format: `pull_chunk<std::string>(str_chunk, stamps, 0.0)`.
- For each sample row: compute `ts_ns` per the selected mode (§7.1); build a
  `BoundFieldValue[]` with native-typed `ValueRef`s (string channels push
  `std::string_view` into the row's owned strings, valid for the `appendBoundRecord`
  call); `writeHost().appendBoundRecord(topic, {ts_ns}, fields)`.
- On `appendBoundRecord` error, `reportMessage(kError, …)` and return `unexpected`.

### 6.3 `onStop()`
Reset all inlets (releases each inlet's receiver thread and socket), clear the
inlet vector, clear cached handles. Idempotent.

### 6.4 Topic / field naming
- **Topic** = the stream's `name`. If two *selected* streams share a name, append
  ` (source_id)` (or a numeric suffix if `source_id` is also empty) to disambiguate.
- **Field** = channel label (or `channel_<i>` fallback).
- This replaces PJ3's flat `source_id/type/label` string key with the SDK's
  topic-groups-fields structure.

## 7. Config JSON schema

```json
{
  "streams": [
    { "source_id": "BioSemi", "name": "BioSemi", "type": "EEG" }
  ],
  "timestamp_mode": "sync"    // "sync" | "raw" | "receiver"
}
```

- `streams[]`: the selected identities (as saved by the dialog).
- `timestamp_mode`: default `"sync"` when absent.

### 7.1 Timestamp modes

Let `s` = LSL sample stamp (seconds, sender `local_clock` domain), `tc` =
`time_corr_s`, `off` = `epoch_offset_ns`.

| Mode | `ts_ns` | Notes |
|---|---|---|
| `sync` (default) | `int64((s + tc) × 1e9) + off` | Absolute, epoch-aligned, multi-stream-synced. If `s == 0` (inlet reported no stamp), fall back to `receiver`. |
| `raw` | `int64(s × 1e9)` | Parity with the original. Arbitrary offset, streams not aligned. |
| `receiver` | `now_epoch_ns()` at drain time | Robust; loses precise inter-sample spacing and cross-stream sync. Same value applied to all samples drained in one `onPoll` for that inlet. |

## 8. Pure, testable helpers (`lsl_conversions.hpp`)

Factored out so the tricky logic is unit-testable without a live LSL network:

- `PJ::PrimitiveType mapChannelFormat(lsl_channel_format_t)` — LSL format → SDK type
  (`cft_float32→kFloat32`, `cft_double64→kFloat64`, `cft_int8/16/32/64→kInt*`,
  `cft_string→kString`, `cft_undefined→` sentinel/skip).
- `std::vector<std::string> channelLabels(const lsl::stream_info&)` — XML label
  extraction with `channel_<i>` fallback and correct count. (Takes the info; the
  XML-walking is the part with edge cases — empty desc, missing labels, fewer
  labels than channels.)
- `int64_t computeTimestampNs(mode, s, tc, off, now_epoch_ns)` — the §7.1 table as a
  pure function.
- `std::string topicNameFor(name, source_id, disambiguation_index)` — §6.4 naming.
- `TimestampMode parseTimestampMode(std::string_view)` / `toString(TimestampMode)`.

## 9. Dependency & build integration

- **`data_stream_lsl/conanfile.py`**: `requires = (plotjuggler_sdk/<SDK_VERSION>,
  gtest/1.17.0, nlohmann_json/3.12.0, liblsl/1.16.2)`, `default_options
  {"*:shared": False}`.
- **`data_stream_lsl/CMakeLists.txt`**: `find_package(LSL REQUIRED)` +
  `find_package(nlohmann_json REQUIRED)`; link `plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json LSL::lsl pj_streaming_utils`; `pj_embed_ui`,
  `pj_embed_manifest`, `pj_emit_plugin_manifest(FAMILY data_source …)`; a
  `lsl_source_test` executable linking `plotjuggler_sdk::plugin_host` + `LSL::lsl`.
- **Root `CMakeLists.txt`**: add the plugin subdirectory alongside the other
  streamers.
- **Aggregate `conanfile.py`**: add `liblsl/1.16.2` to the superset `requires`.
- **`README.md`**: add a plugin badge-table row and a `liblsl | 1.16.2 |
  data_stream_lsl` deps-table row.
- **CI**: add `data_stream_lsl` to the per-plugin build matrix (Linux/Windows/macOS —
  `liblsl` ships ConanCenter binaries for all three; verify availability in the
  JFrog remote and fall back to build-from-source if needed).

## 10. Testing strategy

- **Unit (`tests/lsl_source_test.cpp`, gtest):** cover every helper in §8 —
  format→type table (all formats incl. undefined/string), the three timestamp
  formulas (incl. `sync` `s==0` fallback and the `+off/+tc` arithmetic), label
  extraction (normal, empty desc, missing/short labels), topic disambiguation.
- **Optional integration (guarded, not in the default `ctest` gate):** an
  `lsl::stream_outlet` in-process publisher → `resolve_streams` → open inlet →
  `pull_chunk` round-trip asserting sample values/order. Skipped if LSL multicast is
  unavailable on the CI runner.
- **Manual smoke:** `test_scripts/lsl_publisher.py` (ported/extended from the
  original) emits one numeric stream + one marker stream; load in `pj_proto_app` or
  PJ4 and confirm both appear (numeric plots + string markers) and that the three
  timestamp modes behave as specified.

## 11. Feature-parity audit (porting_guide §0)

| Original feature | Ported? | Where |
|---|---|---|
| Stream discovery list | Yes (richer table) | `LslDialog::resolveStreams` |
| Periodic refresh (1 s) | Yes | `LslDialog::onTick` |
| Preserve selection across refresh | Yes (text-keyed) | `LslDialog` selection merge |
| Select All | Yes | `buttonSelectAll` |
| Multiple simultaneous streams | Yes | `LslSource` inlet vector |
| Channel labels from XML desc + `channel_N` fallback | Yes | `channelLabels()` |
| `source_id/type` prefixing | Reworked → topic=name, field=label | §6.4 |
| Numeric channels | Yes (native types) | `onPoll` |
| **String channels** | **Added** (original dropped them) | `kString` path |
| **Time-corrected timestamps** | **Added** + user-selectable | §7.1 |
| Sample publisher script | Yes | `test_scripts/lsl_publisher.py` |
| liblsl submodule | Replaced by Conan `liblsl/1.16.2` | §9 |

## 12. Future work (explicitly deferred)

- Reconnect/recover a selected stream that is offline at start or drops mid-session.
- Periodic `time_correction()` refresh (single housekeeping thread) for long runs.
- Per-channel unit/type metadata surfaced to the host.
- Full int64 precision path (native-typed `pull_chunk`) if a use case needs it.

## 13. Open sub-decisions (defaulted, override if desired)

- Topic name = stream `name` (disambiguated by `source_id` on collision).
- Discovery widget = multi-column **table** (not a flat list).
- Numeric pull path uses `pull_chunk<double>` then casts to native `PrimitiveType`
  (single code path) rather than templating per native C type.
