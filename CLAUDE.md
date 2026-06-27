# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

PlotJuggler official plugin collection — 12 plugins (CSV, Parquet, ULog, MCAP, JSON, Protobuf, ROS, DataTamer, ZMQ, MQTT, Foxglove Bridge, PJ Bridge) ported to a new SDK. These are **mechanical translations** of the original PlotJuggler plugins, not rewrites.

**Before doing any work, read `porting_guide.md` in its entirety.** It defines the porting philosophy, mandatory workflow, and known pitfalls. The porting gap analysis is tracked in `PORTING_PLAN.md`.

**Exception — `toolbox_anomaly_detector` + `tools/anomaly_runner` (Task F) are NEW, not ports.** They have no PJ3 equivalent, so the porting guide/plan don't apply to them. Engineers author Luau anomaly-detection rules that emit plot markers; the same rule can run two ways:

- **GUI plugin** (`toolbox_anomaly_detector/`): a **host-driven, Luau-FREE** toolbox. It submits the rule to the PlotJuggler host via the unified SDK service **`pj.data_processors.v1`** (`kind="markers"`), and the HOST runs it (and re-runs it as data changes) and publishes the markers; the live preview is host-driven too (an ephemeral generator read back through the object store). So the plugin carries **no script engine** — it links only `core/anomaly_helpers` (the Luau-free builtins + JSON report/rule helpers).
- **Headless CLI** (`tools/anomaly_runner/`): a **standalone** analyzer that runs the rule **in-process** via its own engine (`core/anomaly_core`, which adds `runAnomalyScript` on the **shared Luau engine `pj_scripting_core`** — the same engine PlotJuggler uses for filters; `bandPower` FFT via kissfft lives there). CSV/MCAP in → JSON report + exit code out, for CI.

Both share the **same Lua rule syntax** and emit the **same `PlotMarkers`**, and the GUI host and the CLI run the rule through the **same `runMarkerScript`** core, so a rule authored in the GUI produces identical markers in CI ("GUI == headless") — only *who invokes the engine* differs (the host vs. the standalone runner; the GUI plugin itself runs nothing). The two-layer split (`anomaly_helpers` Luau-free + `anomaly_core` engine) is what lets the GUI plugin stay engine-free. The runner can deliver the report to webhook/email/command sinks (`--notify`, via libcurl; `tools/anomaly_runner/notify.{hpp,cpp}`), and `tools/anomaly_runner/deploy/watch.sh` screens every uploaded MCAP automatically. See `toolbox_anomaly_detector/README.md` (covers BOTH GUI and headless usage). Build: `./build.sh toolbox_anomaly_detector` and `./build.sh tools/anomaly_runner`, then copy the artifacts into `build/all/Release/bin/`.

## Build Commands

### Standalone (requires Conan 2.x)

```bash
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build
```

### As subdirectory of plotjuggler_sdk

```bash
cd ~/ws_plotjuggler/plotjuggler_sdk
./build.sh          # RelWithDebInfo
./build.sh --debug  # Debug + ASAN
```

### Run Tests

```bash
ctest --test-dir build                  # all tests
ctest --test-dir build -R csv_parser    # single test by name
```

Tests exist for: csv, json, protobuf, data_tamer, ros, ulog.

### Smoke Test with pj_proto_app

```bash
./build/pj_proto_app/pj_proto_app \
  --plugin-dir ./build/pj_ported_plugins/bin/ \
  --load /path/to/file.csv \
  --plot 3 --screenshot /tmp/test.png
```

All plugin `.so` files build into `build/pj_ported_plugins/bin/`.

## Code Style

- C++20, Google-based clang-format (2-space indent, 120 col limit, `InsertBraces: true`)
- Pre-commit hooks enforce clang-format and standard checks
- Compiler warnings: `-Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic`

## Architecture

### Plugin Types

Each plugin is a shared library (`.so`) loaded at runtime:

- **DataSource plugins** — file importers (`FileSourceBase`) or streaming sources (`StreamSourceBase`)
- **MessageParser plugins** — decode raw bytes into named fields (`MessageParserPluginBase`)

Export macros: `PJ_DATA_SOURCE_PLUGIN(Class, manifest_json)`, `PJ_MESSAGE_PARSER_PLUGIN(Class, manifest_json)`, `PJ_DIALOG_PLUGIN(DialogClass)`.

### Dual-mode CMake

The top-level CMakeLists.txt supports two modes:
1. **Subdirectory mode** — when `TARGET plotjuggler_sdk::plugin_sdk` already exists (built inside the plotjuggler_sdk repo, which provides the namespaced alias)
2. **Standalone mode** — `find_package(plotjuggler_sdk CONFIG REQUIRED)` against the Conan package from the plotjuggler cloudsmith remote; all other deps via Conan

Plugin CMakeLists.txt files link `plotjuggler_sdk::plugin_sdk` (plugin .so) and `plotjuggler_sdk::plugin_host` (test executables) — same target names work in both modes.

The core version is **not** pinned in CMake — `find_package` resolves whatever Conan installed. The requirement is pinned in **one** place: the top-level `SDK_VERSION` file (an exact version, e.g. `0.6.0`), which the root `conanfile.py` and every plugin's `conanfile.py` read live, and to which the `extern/plotjuggler_core` git submodule is pinned (`v<version>`). Retarget in one step: `python3 scripts/bump_core_version.py 0.6.1` (writes `SDK_VERSION` and moves the submodule); `python3 scripts/bump_core_version.py --check` guards that they agree in CI.

**Repository & package rename (core `v0.6.0`):** the SDK was renamed `plotjuggler_core` → [`plotjuggler_sdk`](https://github.com/PlotJuggler/plotjuggler_sdk) — GitHub repo, Conan package, and CMake identity all move together. Recipes require `plotjuggler_sdk/<version>`; CMake uses `find_package(plotjuggler_sdk)` and links `plotjuggler_sdk::base|plugin_sdk|plugin_host`. The single thing that keeps the old name is the submodule mount point, `extern/plotjuggler_core` (a local directory, not the package). The upstream SDK recipe (`name`, `cmake_file_name`, `cmake_target_name`) and the cloudsmith package are renamed on the SDK side; this repo only consumes the new name.

### Dialog System

Plugins with UI subclass `PJ::DialogPluginTyped` and use real `.ui` files (Qt Creator-editable). CMake's `pj_embed_ui()` compiles `.ui` XML into a `constexpr char[]` header — **no Qt dependency at build time**. The `QDialogButtonBox` must be named `"buttonBox"` for the DialogEngine to wire accept/reject signals.

### Data Write API

- `ValueRef` is a variant — push native types (int64, float, bool, string_view), never cast to double
- `NamedFieldValue.name` is `std::string` (owned) — safe to build via concatenation
- Timestamps are absolute nanoseconds (`int64_t`)
- `writeHost().appendRecord(topic, ts, fields)` for direct ingest
- Columns auto-create on first non-null value; pre-registration optional but recommended when schema is known

### Key Dependencies

| Source | Packages |
|--------|----------|
| Conan (cloudsmith) + `extern/plotjuggler_core` submodule fallback | plotjuggler_sdk (`plotjuggler_sdk::plugin_sdk`, `::plugin_host`) |
| Conan (conancenter) | nlohmann_json, mcap, arrow/parquet, paho-mqtt-cpp, cppzmq, protobuf, zstd, date, ixwebsocket, asio, libsodium, pybind11, cpython, gtest, libcurl (`anomaly_runner` webhook/email notifications) |
| Conan (plotjuggler remote) | pj_scripting_core (the shared Luau marker engine; carries Luau + kissfft) — linked by the Anomaly Detector toolbox + `anomaly_runner` |
| CPM | ulog_cpp, rosx_introspection, data_tamer (plugin-private deps only) |
| Optional | Qt 6 (WebSockets, Network) — only for foxglove_bridge and pj_bridge |

## Porting Rules (Summary)

- Every code path in the original must have a corresponding code path in the port
- Do not optimize, simplify, or "improve" the original's behavior
- If the SDK lacks a capability, extend it or ask — never silently drop features
- Before claiming done, produce a feature audit table (see porting_guide.md §0)
