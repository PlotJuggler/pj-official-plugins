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
cd "${PLOTJUGGLER_SDK_DIR:?set it to your plotjuggler_sdk checkout}"
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

Export macros: `PJ_DATA_SOURCE_PLUGIN(Class, manifest_json)`, `PJ_MESSAGE_PARSER_PLUGIN(Class, manifest_json)`, `PJ_DIALOG_PLUGIN(DialogClass, manifest_json)` (two-arg form requires plotjuggler_sdk >= 0.18.0 on MSVC).

### Dual-mode CMake

The top-level CMakeLists.txt supports two modes:
1. **Subdirectory mode** — when `TARGET plotjuggler_sdk::plugin_sdk` already exists (built inside the plotjuggler_sdk repo, which provides the namespaced alias)
2. **Standalone mode** — `find_package(plotjuggler_sdk CONFIG REQUIRED)` against the Conan package from the shared PlotJuggler JFrog remote; all other deps via Conan

Plugin CMakeLists.txt files link `plotjuggler_sdk::plugin_sdk` (plugin .so) and `plotjuggler_sdk::plugin_host` (test executables) — same target names work in both modes.

The core version is **not** pinned in CMake — `find_package` resolves whatever Conan installed. The requirement is pinned in **one** place: the top-level `SDK_VERSION` file (an exact version, e.g. `0.6.0`), which the root `conanfile.py` and every plugin's `conanfile.py` read live. When no prebuilt Conan package is available, `scripts/ensure_core.sh` clones the matching `v<version>` tag and builds it with `conan create`. Retarget with `python3 scripts/bump_core_version.py 0.6.1`; `python3 scripts/bump_core_version.py --check` guards against stray literal pins in recipes.

**Repository & package rename (core `v0.6.0`):** the SDK was renamed `plotjuggler_core` → [`plotjuggler_sdk`](https://github.com/PlotJuggler/plotjuggler_sdk) — GitHub repo, Conan package, and CMake identity all move together. Recipes require `plotjuggler_sdk/<version>`; CMake uses `find_package(plotjuggler_sdk)` and links `plotjuggler_sdk::base|plugin_sdk|plugin_host`. The upstream SDK recipe (`name`, `cmake_file_name`, `cmake_target_name`) and the JFrog package are renamed on the SDK side; this repo only consumes the new name.

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
| Conan (JFrog), source fallback via `scripts/ensure_core.sh` | plotjuggler_sdk (`plotjuggler_sdk::plugin_sdk`, `::plugin_host`) |
| Conan (JFrog cache → ConanCenter) | nlohmann_json, mcap, arrow/parquet, paho-mqtt-cpp, cppzmq, protobuf, zstd, date, ixwebsocket, asio, kissfft, lua, sol2, libsodium, pybind11, cpython, gtest, libcurl (`anomaly_runner` webhook/email notifications) |
| Conan (JFrog) | pj_scripting_core (the shared Luau marker engine; carries Luau + kissfft) — linked by the Anomaly Detector toolbox + `anomaly_runner` |
| CPM | ulog_cpp, rosx_introspection, data_tamer (plugin-private deps only) |
| Optional | Qt 6 (WebSockets, Network) — only for foxglove_bridge and pj_bridge |

## Release Process

Each plugin's `manifest.json` `version` field is the single source of truth for its release
version. Releases are **not** created manually on GitHub — pushing a tag creates them automatically.

- **Cut a release:** `python3 scripts/release_extension.py <source_dir|extension_id> [--bump patch|minor|major | --version X.Y.Z] [--submit-to-registry]`.
  It reads/updates `manifest.json`, commits the bump (`chore(<dir>): bump version to X.Y.Z`),
  creates an **annotated** tag `<source_dir>/vX.Y.Z` whose message is JSON metadata
  (`{"extension_id", "version", "auto_submit_to_registry"}`), and pushes both the commit and the tag.
  The manifest-bump commit and the tag push are two separate git operations — if only the tag lands
  upstream (e.g. a rejected/reverted commit push), the in-tree manifest silently drifts behind the
  already-released version. Always confirm the bump commit is actually on the target branch, not
  just the tag.
- **What pushing the tag does:** any tag matching `*/v*` triggers `.github/workflows/build-release.yml`,
  which resolves the specific plugin from the tag name (`scripts/release_tools.py resolve-build-scope`),
  builds+tests just that plugin across linux/macos/windows (x86_64 + arm), verifies the built
  artifact's version against the tag (`verify-version-consistency`), packages a zip + sha256, and
  uploads them via `softprops/action-gh-release@v2` — **this step is what creates the GitHub Release**;
  there is no separate manual release-creation step.
- **Registry submission:** if the tag's annotation has `"auto_submit_to_registry": true`, a follow-up
  `submit-to-registry` job runs `scripts/submit_to_registry.py` to open a PR against the extension
  registry with the built artifact's checksums.
- **Version format:** must be 3-part semver (`SEMVER_REGEX` in `scripts/release_tools.py`,
  `^\d+\.\d+\.\d+(-pre)?(+build)?$`) — a two-part string like `"1.0"` fails manifest validation and
  `release_extension.py` will refuse to tag it.
- **Force-recreating a tag** (`--force`) invalidates existing registry checksums and breaks
  installations pinned to the old artifact — treat as a last resort, not a routine fix.

## Porting Rules (Summary)

- Every code path in the original must have a corresponding code path in the port
- Do not optimize, simplify, or "improve" the original's behavior
- If the SDK lacks a capability, extend it or ask — never silently drop features
- Before claiming done, produce a feature audit table (see porting_guide.md §0)
