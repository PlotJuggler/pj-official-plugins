# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

PlotJuggler official plugin collection — 12 plugins (CSV, Parquet, ULog, MCAP, JSON, Protobuf, ROS, DataTamer, ZMQ, MQTT, Foxglove Bridge, PJ Bridge) ported to a new SDK. These are **mechanical translations** of the original PlotJuggler plugins, not rewrites.

**Before doing any work, read `porting_guide.md` in its entirety.** It defines the porting philosophy, mandatory workflow, and known pitfalls. The porting gap analysis is tracked in `PORTING_PLAN.md`.

## Build Commands

### Standalone (requires Conan 2.x)

```bash
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build
```

### As subdirectory of plotjuggler_core

```bash
cd ~/ws_plotjuggler/plotjuggler_core
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
1. **Subdirectory mode** — when `TARGET pj_base` already exists (built inside plotjuggler_core)
2. **Standalone mode** — fetches plotjuggler_core via CPM, deps via Conan

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
| Conan | nlohmann_json, mcap, arrow/parquet, paho-mqtt-cpp, cppzmq, protobuf, zstd, date, gtest |
| CPM | plotjuggler_core (pj_base, pj_dialog_sdk), ulog_cpp, rosx_introspection, data_tamer |
| Optional | Qt 6 (WebSockets, Network) — only for foxglove_bridge and pj_bridge |

## Porting Rules (Summary)

- Every code path in the original must have a corresponding code path in the port
- Do not optimize, simplify, or "improve" the original's behavior
- If the SDK lacks a capability, extend it or ask — never silently drop features
- Before claiming done, produce a feature audit table (see porting_guide.md §0)
