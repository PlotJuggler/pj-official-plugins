# Design Spec — ROS 2 Trace (ros2_tracing / LTTng) Plugin for PlotJuggler 4

- **Date:** 2026-05-30
- **Status:** Approved design, pre-implementation
- **Author:** Davide Faconti (with Claude)
- **Topic:** A PlotJuggler 4 data-source plugin that ingests ROS 2 LTTng/CTF traces produced by `ros2_tracing`, joins the raw events into resolved entities, and exposes timing metrics as PlotJuggler time series — with a forward path to a first-class interval/Gantt view.

---

## 1. Goal & motivation

`ros2_tracing` instruments the ROS 2 core (rclcpp/rcl/rmw) with LTTng tracepoints. Today, analyzing those traces means Python + Jupyter + Bokeh (`tracetools_analysis`) or Eclipse Trace Compass. The goal is to make the most useful trace insights — callback durations, message latency, timer jitter, executor behavior, lifecycle state — available **inside PlotJuggler**, on the same timeline as the rest of a robot's data, with PlotJuggler's interactive plotting.

PlotJuggler natively renders `y(t)`. Roughly half of trace value is naturally `y(t)` (durations/latencies/jitter) and half is interval/Gantt-shaped (callback lanes, executor states, message-flow). This design delivers the `y(t)` half as a self-contained Phase 1, and is structured so the interval half (Phase 2) reuses the same engine once PlotJuggler core grows a first-class interval type.

## 2. Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Scope | Both, **phased** | Timeseries metrics now (fast, PJ-native); Gantt later, designed-for |
| Ingestion | **File + live** from day one | Shared babeltrace2 graph API serves both; one decode/join core |
| CTF reader | **libbabeltrace2** (C API, MIT) | Native, no Python runtime, ideal for in-process live polling |
| Code structure | Standalone **`ros2_trace_model`** lib + thin PJ plugins | UI-agnostic engine reusable by plugin, future Gantt widget, CLI, tests |
| Phase-2 viz | **First-class interval type** in `pj_datastore` + built-in lane view | Benefits all plugins, not just this one; clean `ResolvedEvent → interval` mapping |
| Process | **Git worktrees**, one per repo touched | Standing project rule for cross-repo changes |

## 3. Background: what the trace contains (essential facts)

LTTng provider name is `ros2`; every event is `ros2:<tracepoint>`. Two-phase by design:

- **Init events** (fire once, build a handle→metadata graph): `rcl_init`, `rcl_node_init`, `rcl_publisher_init`, `rcl_subscription_init`, `rclcpp_subscription_init`, `rclcpp_subscription_callback_added`, `rcl_timer_init`, `rclcpp_timer_callback_added`, `rclcpp_timer_link_node`, `rcl_service_init`, `rclcpp_service_callback_added`, `rcl_client_init`, `rclcpp_callback_register` (demangled C++ symbol), `rcl_lifecycle_state_machine_init`.
- **Runtime events** (fire every cycle, tiny payload — just a handle pointer): `callback_start{callback, is_intra_process}`, `callback_end{callback}`, `rcl_publish`/`rclcpp_publish`/`rmw_publish{…, timestamp}`, `rmw_take{rmw_subscription_handle, source_timestamp, taken}`, `rclcpp_executor_wait_for_work{timeout}`, `rclcpp_executor_get_next_ready`, `rclcpp_executor_execute{handle}`, `rcl_lifecycle_transition{state_machine, start_label, goal_label}`.

**The central operation is a join.** A runtime `callback_start.callback` pointer is opaque until joined back through init events:

```
callback_start.callback
  → rclcpp_subscription_callback_added.callback → .subscription
  → rclcpp_subscription_init.subscription       → .subscription_handle
  → rcl_subscription_init.subscription_handle   → .topic_name, .node_handle
  → rcl_node_init.node_handle                   → .node_name, .namespace
callback_start.callback → rclcpp_callback_register.callback → .symbol   (human name)
```
Timer and service callbacks have analogous chains (`rclcpp_timer_callback_added` / `rclcpp_service_callback_added`). Message latency joins `rmw_publish.timestamp` to `rmw_take.source_timestamp` keyed on `(source_timestamp, topic_name)`.

**Reading & licensing:** CTF traces are read with **babeltrace2** (`libbabeltrace2`, **MIT**). LTTng's userspace public headers are MIT; `lttng-ctl` is LGPL-2.1. No GPL linkage is needed to read traces. In babeltrace2's graph model, **file vs live is just the source component**: `source.ctf.fs` (a trace directory) vs `source.ctf.lttng-live` (relay daemon); the muxer + event iterator downstream are identical.

## 4. Architecture

```
              ros2_trace_model   (static C++ lib — NO PlotJuggler dep, NO Qt)
 ┌──────────────────────────────────────────────────────────────────────────┐
 │  TraceSource (abstract)                                                    │
 │    ├─ FsTraceSource    → bt graph: source.ctf.fs          → muxer → iter   │
 │    └─ LiveTraceSource  → bt graph: source.ctf.lttng-live  → muxer → iter   │
 │                                   │ (bt_message → RawEvent{name,ts,fields})│
 │                                   ▼                                        │
 │  Registry   ── init events build handle→entity tables ──                  │
 │     nodes / publishers / subscriptions / timers / services /              │
 │     callbacks: EntityKey → {owner, symbol, topic, node}                   │
 │                                   │                                        │
 │                                   ▼                                        │
 │  Derivers  (runtime events, online state machines)                        │
 │     CallbackDeriver · LatencyDeriver · TimerDeriver ·                      │
 │     ExecutorDeriver · LifecycleDeriver                                     │
 │                                   │                                        │
 │             emits to TWO sink interfaces (the seam):                       │
 │     (1) MetricSampleSink  → Sample{series_path, ts_ns, double|string}      │ ← Phase 1
 │     (2) ResolvedEventSink → ResolvedEvent{entity, kind, t0[, t1], label}   │ ← Phase 2 (Gantt)
 └──────────────────────────────────────────────────────────────────────────┘
            │                                              │
   PJ FileSource plugin (.so)                     PJ StreamSource plugin (.so)
   FileSourceBase::importData():                  StreamSourceBase::onPoll():
     run lib to completion,                         drain lib's sample queue,
     bulk appendArrowStream()                       appendRecord() per batch
```

**The two sink interfaces are the architectural seam.** Phase 1 wires only `MetricSampleSink`. Phase 2 wires `ResolvedEventSink` into the interval write API + lane widget. The library depends on neither PlotJuggler nor Qt.

## 5. `ros2_trace_model` library

A UI-agnostic static library; the only component that links libbabeltrace2. It mirrors the role of `tracetools_analysis`'s `Ros2DataModel`, reimplemented in C++ as **online state machines** so finite (file) and unbounded (live) streams behave identically.

### 5.1 Trace source layer
- `TraceSource` (abstract): owns a babeltrace2 graph and exposes `next() → std::optional<RawEvent>`.
- `FsTraceSource(path)`: graph with `source.ctf.fs`.
- `LiveTraceSource(url)`: graph with `source.ctf.lttng-live`; `session-not-found-action` configurable (`continue` to poll, `end` to stop on session disappearance).
- `RawEvent { std::string_view name; int64_t ts_ns; FieldView fields; }` — thin view over the current `bt_event`; `FieldView` wraps `bt_field_structure_borrow_member_field_by_name_const` with typed getters (`ptr()`, `i64()`, `str()`, `boolean()`). babeltrace2 `abort()`s on precondition violations, so all field access is guarded.

### 5.2 Registry (handle → entity)
Tables populated by init events; keyed by **`EntityKey{ uintptr_t ptr; uint32_t pid; uint32_t host; }`** (not a bare pointer — designed so multi-host traces are not a future rewrite; single-host leaves pid/host zero).

Tables: `nodes`, `rcl_publishers`, `rcl_subscriptions`, `timers`, `services`, `clients`, `callbacks` (`EntityKey → {owner_kind, owner_key, symbol, topic, node}`), `lifecycle_state_machines`, plus the cross-layer links (`timer→node`, `sub→callback`, etc.). `resolve(callback_key) → ResolvedCallback{node, topic_or_timer, symbol}`.

### 5.3 Derivers (runtime → outputs)
Each deriver is an online state machine subscribed to specific tracepoints; emits to both sinks.

| Deriver | Inputs | Join key | Emits |
|---|---|---|---|
| `CallbackDeriver` | `callback_start` / `callback_end` | `callback` ptr | duration `Sample`; `ResolvedEvent{interval, t0..t1}` |
| `LatencyDeriver` | `rmw_publish` / `rmw_take` | `(source_timestamp, topic)` | message-age `Sample` at take time |
| `TimerDeriver` | consecutive timer `callback_start` + `rcl_timer_init.period` | `callback` ptr | jitter `Sample` |
| `ExecutorDeriver` | `wait_for_work` / `get_next_ready` / `execute` | `handle` ptr | wait-duration `Sample`; executor `state` string; `ResolvedEvent{state band}` |
| `LifecycleDeriver` | `rcl_lifecycle_transition` | `state_machine` ptr | `state` string; `ResolvedEvent{state band}` |

Pending-interval state (e.g. open `callback_start` awaiting its `callback_end`) is held in per-deriver maps keyed by `EntityKey`; on `finalize()` (file EOF) any still-open intervals are flushed or discarded per policy.

### 5.4 Sinks
```cpp
struct Sample { std::string series_path; int64_t ts_ns; ValueVariant value; }; // double | string
struct ResolvedEvent { EntityRef entity; EventKind kind; int64_t t0; std::optional<int64_t> t1; std::string label; };
struct MetricSampleSink  { virtual void onSample(const Sample&) = 0; };
struct ResolvedEventSink { virtual void onEvent(const ResolvedEvent&) = 0; };
```
A `Pipeline` object wires `TraceSource → Registry → Derivers → sinks` and exposes `run()` (file, to completion) and `step(budget)` (live, bounded work per call).

## 6. PlotJuggler integration

Two thin plugins in `pj-official-plugins`, both `FileSourceBase`/`StreamSourceBase` with `kCapabilityDirectIngest`. Both implement a `MetricSampleSink` that translates `Sample` → `writeHost()` calls.

### 6.1 Series naming (`/`-rooted, entity-grouped — matches existing PJ convention)
```
/ros2_trace/<node>/callbacks/<symbol_or_id>/duration_ms    scalar  y(t)
/ros2_trace/<node>/callbacks/<symbol_or_id>/active          0→1→0 step   (interval-faked)
/ros2_trace/<node>/timers/<id>/jitter_ms                    scalar  y(t)
/ros2_trace/<node>/executor/wait_ms                         scalar  y(t)
/ros2_trace/<node>/executor/state                           string series
/ros2_trace/<topic>/latency_ms                              scalar  y(t)  (at take time)
/ros2_trace/<node>/lifecycle/state                          string series
```
`<symbol>` is sanitized from `rclcpp_callback_register.symbol` (demangled C++); a stable short id is the fallback when the symbol is unavailable. Cardinality (potentially thousands of callbacks) is mitigated by entity grouping and an event-type filter in the config dialog.

### 6.2 Interval faking (Phase 1) — forward-compatible with Phase 2
A `ResolvedEvent{interval}` is written two ways in Phase 1: a numeric `…/active` step (0 before `t0`, 1 in `[t0,t1)`, 0 after) and/or a `…/state` string series. In Phase 2 the **same** `ResolvedEvent` is written through the new `appendInterval(t0, t1, label)` API and rendered in the lane view — no rework in the model layer.

### 6.3 Write path
- **File:** prefer bulk `writeHost().appendArrowStream(topic, stream, "timestamp")` for the per-series columns (trace sessions reach millions of events); fall back to `appendRecord` for low-volume series.
- **Live:** `onPoll()` drains the model's sample queue and calls `appendRecord` (or `appendBoundRecord` with pre-registered `FieldHandle`s for hot series). Receiver thread never calls `writeHost()`.

### 6.4 Clock alignment (must verify against a real trace)
LTTng's default clock is `CLOCK_MONOTONIC`; babeltrace2 `bt_clock_snapshot_get_ns_from_origin()` returns ns from the clock's origin, which may not be Unix-epoch-aligned, whereas PJ `Timestamp` is int64 ns since Unix epoch. The plugin exposes a **clock-offset option** (auto-detect from the clock class origin where possible, manual override otherwise) so trace series align with other ROS data on the same timeline.

## 7. Live path & threading

```
[traced ROS 2 process] → lttng-sessiond + consumerd → lttng-relayd (live :5344)
    → source.ctf.lttng-live  →  ros2_trace_model  →  StreamSource plugin
```
- `LiveTraceSource` runs the bt graph on a **background receiver thread**; resolved `Sample`s go into a `std::mutex`-guarded queue. `StreamSourceBase::onPoll()` drains the queue and writes — matching the existing UDP streamer pattern and PJ's rule that only the callback thread may touch `writeHost()`.
- Only `is_stop_requested()/notify_state()/request_stop()/report_message()` are called off the callback thread.
- **Documented caveats handled explicitly:** (1) single consumer per session per relay; (2) **init events are lost on late attach** → detect absent init data and surface a clear warning ("create the trace session with `--live` before starting nodes, or use offline"); (3) `ros2 trace` CLI lacks `--live` (as of 2026-05) → dialog documents `lttng create … --live` directly; (4) live latency is sub-second (sub-buffer flush, default 1 s), not sub-ms.

## 8. Config dialog (declarative panel)

PJ4 plugins are Qt-free at the ABI boundary; the only custom UI is the declarative `DialogPluginTyped` path (a Qt `.ui` XML embedded via `pj_embed_ui`, instantiated host-side by `QUiLoader`). The dialog provides: source selection (trace directory path / live URL), clock-offset control, and an event-type/entity filter (callbacks, latency, timers, executor, lifecycle). No custom chart is needed in Phase 1 — derived series render in standard PJ plots.

## 9. Phase 2 (separate spec): first-class interval type

Out of scope for implementation here, but the Phase-1 design reserves the hooks. Phase 2 adds to `plotjuggler_core`/`PJ4`:
1. An **interval record** in `pj_datastore` (`t_start`, `t_end`, `label`) alongside the existing scalar/string columns.
2. A write API (`appendInterval(...)`) on the host views.
3. A built-in **lane/Gantt widget** that renders interval series (and, later, causal arrows for message-flow).

The `ros2_trace_model` library needs **no change** — its `ResolvedEventSink` is already the interval producer. This benefits every plugin that has interval/state data (state machines, Mosaico), not only trace.

## 10. Repos, worktrees & milestones

**Phase 1 touches `pj-official-plugins` only** (single worktree). New tree:
```
pj-official-plugins/ros2_trace/
  model/        # ros2_trace_model static lib (+ unit tests, golden fixture)
  file_source/  # FileSourceBase plugin
  live_source/  # StreamSourceBase plugin
```
Per the standing rule, implementation happens in a dedicated git worktree of `pj-official-plugins` (Phase 2 later gets a separate `plotjuggler_core`/`PJ4` worktree).

**Milestones:**
1. `ros2_trace_model`: babeltrace2 file reader + `Registry` + `CallbackDeriver`, unit-tested against a golden trace. *(No PJ/Qt.)*
2. FileSource plugin: `MetricSampleSink → writeHost()`; load a trace dir; `…/duration_ms` curves appear in PJ.
3. Remaining derivers: latency, timer jitter, executor, lifecycle + step/string faking.
4. `LiveTraceSource` + StreamSource plugin: `lttng-live`, threading discipline, late-attach warning.
5. Config dialog: source/clock/filter controls.

## 11. Testing

- **Library (primary):** commit a **small golden CTF fixture** (tiny talker/listener + a timer, traced once) under `model/test/fixtures/`. Unit-test the join engine directly — assert resolved entities (node/topic/symbol), callback durations, and message latencies — with no PlotJuggler in the loop. This is the main correctness guarantee and is fast because the library is UI-free.
- **Plugin:** load the same fixture through `FileSource` and assert the expected series names/values land in the datastore (reuse PJ4's ctest harness).
- **Live:** validate `LiveTraceSource` against a recorded `lttng-live` session or a mocked bt source; at minimum a documented manual test against a running demo.

## 12. Build & dependencies

- `ros2_trace_model`: plain CMake static lib; links **libbabeltrace2** via pkg-config `babeltrace2` (`apt: libbabeltrace2-dev`). No Qt, no PJ. Pin and document a **minimum babeltrace2 version** (2.0+).
- Plugins: link `ros2_trace_model` + `plotjuggler_core::plugin_sdk`; register with `PJ_DATA_SOURCE_PLUGIN(...)` + `pj_emit_plugin_manifest(FAMILY data_source ...)`; dialog `.ui` embedded with `pj_embed_ui`. The `.so` links no Qt.
- Manifest declares `file_extensions` for the trace directory marker (the host reads it pre-`dlopen`); live source advertises `kCapabilityContinuousStream`.

## 13. Risks & open questions

- **Clock origin/offset** (§6.4) — must be verified against a real trace; misalignment silently de-syncs trace series from other ROS data.
- **libbabeltrace2 availability/version** across target distros; young ABI (no stability guarantee). Mitigation: pin minimum version, isolate all babeltrace2 use in the model lib.
- **Series cardinality** — thousands of callbacks/timers can explode the topic tree; mitigated by grouping + dialog filter, but confirm the datastore + tree UI scale.
- **Live late-attach** — without startup init events the joins fail; handled by detection + warning, but it is an inherent LTTng-live limitation.
- **`rclpy` nodes** lack `callback_start`/`callback_end` instrumentation → Python-node callback timing is unavailable (upstream limitation; document it).
- **Interval flush policy** at file EOF / live stop for still-open intervals — define (discard vs close-at-last-timestamp).

## 14. References

- ros2_tracing: https://github.com/ros2/ros2_tracing — tracepoint defs in `tracetools/include/tracetools/tp_call.h`
- tracetools_analysis (data-model reference): https://github.com/ros-tracing/tracetools_analysis
- Paper: *ros2_tracing: Multipurpose Low-Overhead Framework for Real-Time Tracing of ROS 2* — arXiv:2201.00393
- Multi-host / message-flow: arXiv:2204.10208; https://christophebedard.com/ros-tracing-message-flow/
- babeltrace2: https://babeltrace.org/docs/ — `source.ctf.fs`, `source.ctf.lttng-live`
- CTF spec: https://diamon.org/ctf/
- PJ4 plugin API (local): `pj_base/include/pj_base/sdk/plugin_data_api.hpp`, `data_source_patterns.hpp`, `data_source_plugin_base.hpp`
