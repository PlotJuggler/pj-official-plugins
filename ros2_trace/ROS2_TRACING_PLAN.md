# ROS 2 Tracing → PlotJuggler Plugin — Working Plan

Status doc for the `ros2_trace` plugin. Read this first to resume work.

- **Branch:** `dfaconti/ros2-trace-plugin` (worktree of `pj-official-plugins`, off `main`)
- **Last updated:** 2026-05-30
- **Full design spec:** `ros2_trace/DESIGN_SPEC.md`
- **Original implementation plan:** `/home/davide/.claude/plans/twinkling-leaping-prism.md`

---

## 1. Goal

Make `ros2_tracing` (ROS 2's LTTng/CTF instrumentation) usable **inside PlotJuggler**: load a trace and see callback durations, message latency, timer jitter, executor behavior, and lifecycle state as time-series, on the same timeline as the robot's other data — instead of Jupyter/`tracetools_analysis` or Trace Compass.

**Phased:**
- **Phase 1 (in progress):** timeseries metrics as normal PJ `y(t)` curves + faked intervals (step/string signals). File + live ingestion.
- **Phase 2 (future, separate spec):** a first-class **interval type** in `pj_datastore` + a built-in lane/Gantt widget, so callback/executor/lifecycle intervals and message-flow render as real Gantt lanes. The engine already emits the data for this (see `ResolvedEventSink`), so Phase 2 is additive — no model rewrite.

---

## 2. Locked design decisions

| Decision | Choice | Why |
|---|---|---|
| Scope | Both, **phased** (timeseries now, Gantt later) | PJ renders y(t) natively; intervals need core work |
| Ingestion | **File + live** from day one (shared core) | babeltrace2: file vs live is just the source component |
| CTF reader | **libbabeltrace2** C API (MIT) | native, no Python runtime, works for live polling |
| Code structure | standalone **`ros2_trace_model`** lib + thin PJ plugins | UI-agnostic engine reusable by plugin / future Gantt / CLI / tests |
| Phase-2 viz | **first-class interval type** in `pj_datastore` | benefits all plugins, clean ResolvedEvent→interval mapping |
| Executor attribution | **per-CPU** (`cpu_id` from CTF packet context) | executor events carry no node/thread; cpu is the real disambiguator |
| Process | **git worktrees**, one per repo | standing project rule |

**Key constraint:** PJ4 has NO first-class interval type and NO plugin hook for a custom `QWidget`. So Phase 1 fakes intervals as a `…/active` 0→1→0 numeric step and `…/state` string series. The `ResolvedEventSink` interface exists as the seam for Phase 2.

---

## 3. Architecture

```
        ros2_trace_model   (static C++ lib — NO PlotJuggler dep, NO Qt)
  TraceSource (abstract)
    ├─ FsTraceSource    → bt graph: source.ctf.fs        → muxer → events   [DONE]
    └─ LiveTraceSource  → bt graph: source.ctf.lttng-live → muxer → events   [TODO M4]
                                   │ RawEvent{tp, ts_ns, fields, cpu}
                                   ▼
  Registry   ── init events build handle→entity tables ──                    [DONE]
     resolveCallback(sub/timer/service), topicForRmwSubscription,
     nodeForStateMachine, timerPeriodForCallback
                                   ▼
  Derivers (online state machines)                                           [DONE]
     CallbackDeriver (duration_ms + active step) · LatencyDeriver ·
     TimerDeriver (jitter) · LifecycleDeriver (state) · ExecutorDeriver (per-CPU)
                                   │
        emits to TWO sinks (the seam):
     (1) MetricSampleSink  → Sample{series, ts_ns, double|string}   ← Phase 1 [DONE]
     (2) ResolvedEventSink → ResolvedEvent{kind, entity, t0,[t1], label} ← Phase 2 [defined, unused]
                                   │
        Pipeline.run(TraceSource) drives source→registry→derivers→sinks      [DONE]
            │
   ┌────────┴─────────┐
   ▼                  ▼
 FileSource plugin   StreamSource plugin
 TraceFileSource     (TODO M4)
 [DONE]
```

The **two sink interfaces are the architectural seam.** Phase 1 wires only `MetricSampleSink`. The plugin's `WriteHostSink` implements it and writes to PlotJuggler.

---

## 4. Status — what's DONE (8 commits: 731a74d → 1607e8b, 18 GTest green, strict -Werror)

| Component | What | Commit |
|---|---|---|
| `Registry` | node + subscription + timer + service callback resolution; rmw-sub→topic, state-machine→node, timer-period | 731a74d, de31a84, d4e1d4c |
| `CallbackDeriver` | `callbacks/<sym>/duration_ms` + `callbacks/<sym>/active` (0→1→0 step) | 731a74d, 54cce7c |
| `LatencyDeriver` | `/ros2_trace/<topic>/latency_ms` (take-time − source_timestamp) | dcb2fb8 |
| `TimerDeriver` | `timers/<id>/jitter_ms` (interval − nominal period) | d4e1d4c |
| `LifecycleDeriver` | `lifecycle/state` (string series) | d4e1d4c |
| `ExecutorDeriver` | per-CPU `executor/cpu<N>/{state,wait_ms}` | 54cce7c |
| `Pipeline` + `TraceSource` | runs all derivers end-to-end (tested w/ in-memory source) | de31a84, 54cce7c |
| `classifyTracepoint` | `ros2:*` name → `Tp` (30 tracepoints) | 0d23a26 |
| `FsTraceSource` | libbabeltrace2 `source.ctf.fs` reader; ts/fields/cpu_id decode; error path | 0d23a26 |
| **`TraceFileSource` plugin** | `FileSourceBase`; `libros2_trace_file_source.so` + `.pjmanifest.json` | 1607e8b |

Produced series (PlotJuggler tree):
```
/ros2_trace/<node>/callbacks/<symbol>/duration_ms   (double)
/ros2_trace/<node>/callbacks/<symbol>/active        (double 0/1 step)
/ros2_trace/<node>/timers/<symbol>/jitter_ms        (double)
/ros2_trace/<topic>/latency_ms                      (double)
/ros2_trace/<node>/lifecycle/state                  (string)
/ros2_trace/executor/cpu<N>/state                   (string)
/ros2_trace/executor/cpu<N>/wait_ms                 (double)
```

---

## 5. TODO

### M4 — Live path (`source.ctf.lttng-live` + `StreamSourceBase`)
- `LiveTraceSource(url)` in `model/`: same babeltrace2 graph as `FsTraceSource` but with the `lttng-live` source component (`net://HOST/host/TGTHOST/SESSION`, port 5344). Make the simple-sink / graph drive **incrementally** (`bt_graph_run_once`) and buffer a batch, since live is unbounded — do NOT run-to-completion. Set `session-not-found-action`.
- `ros2_trace/live_source/` plugin: `StreamSourceBase` (`onStart`/`onPoll`/`onStop`). Background receiver thread runs the bt graph → pushes `Sample`s into a mutex-guarded queue; `onPoll()` drains and calls `writeHost().appendRecord`. **Only the callback thread may touch writeHost()** (mirror `data_stream_udp/udp_source.cpp`).
- Handle caveats: (1) one live consumer per session; (2) **init events are lost on late attach** → detect missing init data and warn ("create session with `--live` before launching nodes"); (3) `ros2 trace` lacks `--live` (as of 2026-05) → use `lttng create … --live` directly; (4) latency sub-second (`--live-timer`, default 1 s).

### M5 — Config dialog (`DialogPluginTyped`) + trace-dir picker UX
- The file loader's `manifest.json` has `"file_extensions": []` because a **trace is a directory**, not a file. PJ's file picker matches by extension, so the clean fix is a dialog with a **directory picker** (and a "live URL" field). See `data_load_csv/csv_dialog.{hpp,cpp}` + `dataload_csv.ui` + `pj_embed_ui()` for the pattern. The `QDialogButtonBox` must be named `"buttonBox"`.
- Dialog fields: trace dir / live URL · clock offset · event-type/entity filter (callbacks/latency/timers/executor/lifecycle). Add `kCapabilityHasDialog` + `getDialog()` to the plugin(s).

### Validation — end-to-end (needs a real ROS 2 machine; NOT possible in this sandbox)
- Generate a trace: `ros2 trace -s my_session` (or `lttng create; lttng enable-event -u 'ros2:*'; lttng start; <run nodes>; lttng stop; lttng destroy`).
- Load the trace **directory** (or its `metadata` file) via the plugin; confirm the series above appear with sane values; cross-check a couple against `ros2 trace-analysis` / `tracetools_analysis`.
- **Verify clock alignment** (§7): LTTng default is `CLOCK_MONOTONIC`; `bt_clock_snapshot_get_ns_from_origin` may not be Unix-epoch-aligned, while PJ `Timestamp` is. Add a clock-offset option if trace series don't line up with other ROS data.
- Commit a small golden CTF fixture under `model/test/fixtures/` and add the deferred `FsTraceSource` round-trip integration test (assert resolved entities + a couple of metric values).

### Phase 2 (separate spec) — first-class interval type
- Add interval record (`t_start`, `t_end`, `label`) to `pj_datastore` + `appendInterval()` write API + a lane/Gantt widget in `plotjuggler_core`/`PJ4` (separate worktree). Wire `ResolvedEventSink` into it. Message-flow causal arrows + aggregate views (histograms, flame graphs) are Phase 2+.

### Smaller TODOs / risks
- `FsTraceSource` buffers ALL events then yields (fine for files; memory-heavy for huge traces). Consider streaming (`bt_graph_run_once`) if needed — the same machinery M4 needs.
- Series-name cardinality: thousands of callbacks → many topics; confirm datastore + tree UI scale; consider grouping.
- `rclpy` (Python) nodes lack `callback_start`/`callback_end` → their callback timing is unavailable (upstream limitation; document in dialog).
- Multi-host traces: `EntityKey{ptr,pid,host}` already supports it, but the reader currently leaves pid/host = 0. Fill from trace env/stream if multi-host support is wanted.
- Pause for live: `StreamSourceBase` doesn't wire `pause()/resume()`; override on `DataSourcePluginBase` + add `kCapabilitySupportsPause` if needed.

---

## 6. Build & test

**Library only (fast, dep-free TDD loop — needs only system GTest + babeltrace2):**
```bash
cd ros2_trace/model
cmake -S . -B /tmp/m1build && cmake --build /tmp/m1build && ctest --test-dir /tmp/m1build --output-on-failure
```

**Full plugin (needs conan; single-plugin build):**
```bash
cd <repo root>             # pj-official-plugins worktree
./build.sh ros2_trace      # conan install (cppstd=20) + cmake -DPJ_BUILD_PLUGIN=ros2_trace + ninja
# rebuild only:
cmake --build build/ros2_trace/Release --parallel
# artifacts: build/ros2_trace/Release/bin/libros2_trace_file_source.so (+ .pjmanifest.json)
```

**System deps:** `sudo apt install libbabeltrace2-dev lttng-tools` (babeltrace2 is a system pkg-config dep, NOT conan).

---

## 7. Key technical facts (learned the hard way — keep these)

- **SDK namespaces:** `PJ::sdk::TopicHandle`, `PJ::sdk::NamedFieldValue`, `PJ::sdk::SourceWriteHostView` (the `sdk::` is easy to forget). But `PJ::Timestamp`, `PJ::Span`, `PJ::Status`, `PJ::okStatus()`, `PJ::unexpected()`, `PJ::FileSourceBase`, `PJ::kCapability*`, `PJ::DataSourceMessageLevel` are in `PJ`.
- **Write API:** `writeHost().ensureTopic(name) -> Expected<TopicHandle>`; `appendRecord(topic, PJ::Timestamp{ns}, PJ::Span<const NamedFieldValue>(ptr,n))`. `NamedFieldValue{.name=std::string, .value=ValueRef}`. **Push native ValueRef types (double / string_view), never cast to double.** Columns auto-create on first write. Timestamps are int64 ns.
- **Registration:** `PJ_DATA_SOURCE_PLUGIN(Class, kManifestVar)` at file scope; `pj_embed_manifest(target HEADER … VAR_NAME …)` reads `manifest.json` → constexpr `char[]`; `pj_emit_plugin_manifest(target FAMILY data_source MANIFEST_FILE …)` writes the sidecar (`abi_major:5`). No Qt in the `.so`.
- **Series→topic/field:** PJ joins topic + field with `/`. The plugin splits `Sample.series` at the LAST `/` → topic = prefix, field = leaf. Reproduces the full path and groups a callback's duration_ms+active under one node.
- **clang-format pre-commit hook:** Google style, `InsertBraces: true`, 120 cols, 2-space. On the FIRST `git commit` it reformats files and **aborts** (exit 1); just `git add` + commit again. Write code in that style to minimize churn.
- **conan:** single-plugin build needs the plugin's own `conanfile.py` (minimal deps); requires `-s compiler.cppstd=20` (arrow needs C++20). conan 2.27.1; plotjuggler_core 0.5.1 cached; plotjuggler_core transitively pulls arrow.
- **babeltrace2 reader notes (`fs_trace_source.cpp`):** graph = `source.ctf.fs` → `filter.utils.muxer` → `bt_graph_add_simple_sink_component` (consume callback drains `bt_message_iterator_next`, `bt_message_put_ref` each). Decode: event-class name → `classifyTracepoint`; ts via `bt_message_event_borrow_default_clock_snapshot_const` + `bt_clock_snapshot_get_ns_from_origin`; payload struct members iterated generically (unsigned int→uint64, signed→int64, bool, string; arrays/structs skipped); cpu via packet-context `cpu_id`. `NamedField.name` is an **owned std::string** (CTF names freed with the graph). MIT license. **Multi-output-port source→muxer connection (one port per stream) is untested at runtime** — verify on a real trace.
- **Tracepoint join chains** (Registry resolution): `callback_start.callback` → `rclcpp_{subscription,timer,service}_callback_added.callback` → owner handle → … → node/topic; symbol via `rclcpp_callback_register.callback→symbol`. Latency: `rmw_take` carries `source_timestamp` (= the publisher's `rmw_publish.timestamp`), so latency = take_event_ts − source_timestamp, attributed via `rmw_subscription_handle → topic`.

---

## 8. Files

```
ros2_trace/
  ROS2_TRACING_PLAN.md          ← this file
  conanfile.py                  (plotjuggler_core + gtest + nlohmann_json)
  CMakeLists.txt                (add_subdirectory model, file_source)
  model/                        ros2_trace_model static lib (Qt-free, PJ-free)
    CMakeLists.txt              (dual-mode standalone/subdir; optional pkg-config babeltrace2)
    include/ros2_trace_model/   entity_key, raw_event, registry, sinks, trace_source,
                                callback_deriver, latency_deriver, timer_deriver,
                                lifecycle_deriver, executor_deriver, pipeline, fs_trace_source
    src/                        (.cpp for each)
    tests/                      *_test.cpp (GTest; 18 cases)
  file_source/                  TraceFileSource plugin
    trace_file_source.cpp, manifest.json, CMakeLists.txt
  live_source/                  (TODO M4)
```

Registered in the repo's top `CMakeLists.txt` in both subdirectory-mode and standalone (`PJ_BUILD_PLUGIN`) lists, after `parser_ros`.

---

## 9. References

- ros2_tracing: https://github.com/ros2/ros2_tracing — tracepoints in `tracetools/include/tracetools/tp_call.h`
- tracetools_analysis (data-model reference): https://github.com/ros-tracing/tracetools_analysis
- Papers: arXiv:2201.00393 (ros2_tracing), arXiv:2204.10208 (message-flow / multi-host)
- babeltrace2: https://babeltrace.org/docs/ — `source.ctf.fs`, `source.ctf.lttng-live`, simple sink
- CTF spec: https://diamon.org/ctf/
- PJ SDK headers (conan pkg or `plotjuggler_core/pj_base/include/pj_base/sdk/`): `data_source_patterns.hpp`, `data_source_plugin_base.hpp`, `plugin_data_api.hpp`
- Plugin templates to mirror: `data_load_csv/` (FileSource + dialog), `data_stream_udp/udp_source.cpp` (StreamSource threading)
