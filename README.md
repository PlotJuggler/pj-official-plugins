# PlotJuggler Official Plugins

[![CI Linux](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml)
[![CI Windows](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml)
[![CI macOS](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml)


| Plugin | Type | Description |
|--------|------|-------------|
| [![parser_json](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_json.json)](parser_json/) | MessageParser | JSON message parsing |
| [![parser_protobuf](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_protobuf.json)](parser_protobuf/) | MessageParser | Protobuf message parsing |
| [![parser_ros](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_ros.json)](parser_ros/) | MessageParser | ROS 1/2 message parsing |
| [![parser_data_tamer](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_data_tamer.json)](parser_data_tamer/) | MessageParser | DataTamer schema/snapshot parsing |
| [![parser_influx](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_influx.json)](parser_influx/) | MessageParser | InfluxDB line protocol parsing (Telegraf) |
| [![parser_navmap](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_navmap.json)](parser_navmap/) | MessageParser | EasyNavigation NavMap maps and layer updates in the 3D scene |
| [![parser_arrow](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_arrow.json)](parser_arrow/) | MessageParser | Arrow IPC stream parsing |
| [![data_load_csv](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_csv.json)](data_load_csv/) | DataSource | CSV file loading |
| [![data_load_mcap](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_mcap.json)](data_load_mcap/) | DataSource | MCAP file loading |
| [![data_load_parquet](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_parquet.json)](data_load_parquet/) | DataSource | Parquet file loading |
| [![data_load_ulog](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_ulog.json)](data_load_ulog/) | DataSource | ULog file loading |
| [![data_load_lerobot](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_lerobot.json)](data_load_lerobot/) | DataSource | LeRobot v2.1 dataset loading |
| [![data_load_mp4](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_mp4.json)](data_load_mp4/) | DataSource | MP4 video loading |
| [![data_load_3d](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_3d.json)](data_load_3d/) | DataSource | PLY/PCD/LAS point cloud & mesh loading |
| [![data_load_mf4](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_mf4.json)](data_load_mf4/) | DataSource | ASAM MF4/MDF loading (+ DBC CAN decode) |
| [![data_load_blf](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_blf.json)](data_load_blf/) | DataSource | Vector BLF CAN-log loading (DBC decode) |
| [data_load_candump](data_load_candump/) | DataSource | Linux SocketCAN `candump` log/screen capture loading (DBC or ARUS-CSV decode) |
| [![data_stream_zmq](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_zmq.json)](data_stream_zmq/) | DataSource | ZeroMQ streaming |
| [![data_stream_mqtt](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_mqtt.json)](data_stream_mqtt/) | DataSource | MQTT streaming |
| [![data_stream_foxglove_bridge](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_foxglove_bridge.json)](data_stream_foxglove_bridge/) | DataSource | Foxglove WebSocket bridge |
| [![data_stream_pj_bridge](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_pj_bridge.json)](data_stream_pj_bridge/) | DataSource | PlotJuggler WebSocket bridge |
| [![data_stream_ros2](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_ros2.json)](data_stream_ros2/) | DataSource | ROS 2 topic streaming |
| [![data_stream_udp](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_udp.json)](data_stream_udp/) | DataSource | UDP unicast/multicast streaming |
| [![data_stream_webrtc](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_webrtc.json)](data_stream_webrtc/) | DataSource | WebRTC live H.264 video streaming |
| [![data_stream_dummy](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_dummy.json)](data_stream_dummy/) | DataSource | Synthetic test-signal generator |
| [![data_stream_lsl](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_lsl.json)](data_stream_lsl/) | DataSource | Lab Streaming Layer (LSL) streaming |
| [![toolbox_fft](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_fft.json)](toolbox_fft/) | Toolbox | FFT frequency-domain transform |
| [![toolbox_quaternion](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_quaternion.json)](toolbox_quaternion/) | Toolbox | Quaternion → roll/pitch/yaw |
| [![toolbox_data_exporter](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_data_exporter.json)](toolbox_data_exporter/) | Toolbox | Export selected series to CSV or Parquet |
| [![toolbox_mosaico](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_mosaico.json)](toolbox_mosaico/) | Toolbox | Mosaico cloud server browsing (Arrow Flight) |
| [![toolbox_transform_editor](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_transform_editor.json)](toolbox_transform_editor/) | Toolbox | Lua-based editor for derived series and 2D/3D objects |
| [toolbox_anomaly_detector](toolbox_anomaly_detector/) | Toolbox | Marker Editor: Lua rules → plot markers (anomalies, phases, color by value; shares its engine with the headless runner) |
| [toolbox_assistant_agent](toolbox_assistant_agent/) | Toolbox | LLM assistant driving PlotJuggler's own MCP tools through your Claude Code / Codex CLI (Linux/macOS runtime; PlotJuggler 4.2+) |
| [widget_basic](widget_basic/) | Widget | Gauge, numeric indicator, traffic light and value table dock widgets (links Qt; Linux x86_64 and Windows x64 only) |

[`tools/anomaly_runner`](tools/anomaly_runner/) is a standalone headless CLI that runs the same
anomaly-detection rules without a GUI (CSV/MCAP in, JSON report + exit code out), with optional
webhook/email/command notification; see its [README](tools/anomaly_runner/README.md).

## Building

### Standalone (requires Conan 2.x)

Configure the shared PlotJuggler JFrog Conan remote once per machine. Keeping
it at index 0 lets Conan try cached PlotJuggler and third-party binaries before
falling back to ConanCenter:

```bash
conan remote add plotjuggler-conan \
  https://plotjuggler.jfrog.io/artifactory/api/conan/plotjuggler-conan \
  --force --index=0
conan remote login plotjuggler-conan <user> -p <access-token>
```

Then build:

```bash
# Build the full plugin collection
./build.sh
```

To work on only one plugin, pass the plugin directory:

```bash
./build.sh data_load_csv
```

Run `./build.sh --help` to see the available arguments.

By default, `build.sh` installs the root `conanfile.py` and builds into
`build/all/Release`. With a plugin argument, it installs that plugin's
`conanfile.py`, configures CMake with `-DPJ_BUILD_PLUGIN=<plugin_dir>`, and
builds into `build/<plugin_dir>/Release`.

Each plugin directory has its own `conanfile.py` that lists
`plotjuggler_sdk` plus the plugin's own third-party deps. Keep it in sync
with the plugin's `find_package(... REQUIRED)` calls in `CMakeLists.txt`.
The root `conanfile.py` remains the full-repository dependency set for
local full builds and scheduled CI.

### As subdirectory of plotjuggler_sdk

No extra steps — the parent project's build system handles everything (and
`plotjuggler_sdk::plugin_sdk` / `::plugin_host` resolve to the in-tree
targets, so plugin CMakeLists.txt files write the same `target_link_libraries`
call in both modes):

```bash
cd /path/to/plotjuggler_sdk
./build.sh
```

### Per-plugin status badges

Each plugin badge is a [shields.io endpoint badge](https://shields.io/badges/endpoint-badge)
backed by `<plugin>.json` (`{ label, message: version, color }`) on the orphan `badges` branch, so one
badge carries both the version and the build color. CI publishes them on pushes to `main`:

- The CI Linux `per-plugin-build` matrix emits one badge JSON per leg (version from `manifest.json`,
  color from the leg's outcome); the `update-badges` job commits them to `badges` in one push.
- `data_stream_ros2` is built only by CI ROS2, which writes its own badge from its `update-badge` job.

Both writers share a `badges-branch` concurrency group and resync on conflict
(`scripts/publish_badges.sh`).

## Plugin architecture

Plugins are **declarative** on top of the `plotjuggler_sdk` SDK: a DataSource hands the host a
deferred byte fetcher per message, and a MessageParser declares a table of schema handlers that
produce canonical objects (`sdk::Image`, `sdk::PointCloud`, ...) plus scalar columns. The host
chooses eager vs lazy materialization per message.

See [`PLUGIN_DEVELOPMENT.md`](PLUGIN_DEVELOPMENT.md) for the developer guide.

## WebAssembly build

PlotJuggler's browser build cannot load plugins at run time, so it compiles the plugins it ships
into its own module, from a pinned commit of this repository. Each such plugin carries a
`wasm.cmake` that declares that build with one `pj_wasm_official_plugin()` call (sources,
manifest, `.ui` files, exported plugin and dialog symbols, links) plus any browser-only quirk;
shared `common/` libraries come from their native CMakeLists through `pj_wasm_common_library()`.
The native and Conan builds never read these files.
[`cmake/PjWasmPlugin.cmake`](cmake/PjWasmPlugin.cmake) states the contract with the host build:
the dependency targets it provides and how registrations reach it.

To make a plugin available in the browser:

1. Write its `wasm.cmake` (copy a similar plugin's, such as
   [`parser_json/wasm.cmake`](parser_json/wasm.cmake)).
   It needs no raw sockets, child processes or native middleware, and its export macros
   must name an unqualified class (or use the `_NAMED` variants).
2. In PlotJuggler, add its directory to `PJ_WASM_OFFICIAL_PLUGIN_DIRS`
   (`cmake/PjWasmOfficialPlugins.cmake`) and pin a commit of this repository that has it
   (`cmake/PjWasmDependencies.cmake`); try it before pinning with
   `-DFETCHCONTENT_SOURCE_DIR_PJ_OFFICIAL_PLUGINS=<this checkout>`. A dependency the host
   does not provide yet goes into its `PjWasmDependencies.cmake`, and every new static
   archive needs the symbol-collision audit and notice entry PlotJuggler's `CLAUDE.md` asks for.

This repository's CI does not build the browser app: PlotJuggler's WASM CI does, on the
PR that moves its pin. A change to a browser-built plugin or to `common/` should therefore
be built once with the override above before it merges here.

## Development Checklist

When adding or changing a plugin:

1. Keep `manifest.json` current; the release tag version must match it.
2. Add or update the plugin's `CMakeLists.txt`.
3. Add any Conan dependencies to the plugin's `conanfile.py`.
4. Add new dependencies to the root `conanfile.py` when full-repository builds need them.
5. Add focused tests in the plugin directory when behavior changes.

## Releasing Extensions

Each plugin is independently versioned and released. Pushing a tag builds that plugin on 6 platforms
(Linux x86_64/aarch64, macOS Intel/ARM, Windows x64/ARM64), creates a GitHub Release with the
plugin's notes, and can open a `pj-plugin-registry` PR for the tagged version.

```bash
# Bump manifest.json, commit, push, tag; CI builds and submits to the registry
python3 scripts/release_extension.py foxglove-bridge --bump minor --submit-to-registry

# Manifest already at the target version: tag only
python3 scripts/release_extension.py foxglove-bridge --submit-to-registry
```

### Tag Convention

```
<source_directory>/v<semver>
```

Examples: `data_load_csv/v1.0.6`, `parser_ros/v2.1.0`

The directory before `/v` sets the CI build scope: a `data_load_csv/v1.0.6` tag installs
`data_load_csv/conanfile.py` and configures CMake with `-DPJ_BUILD_PLUGIN=data_load_csv`, through the
same `build.sh` entry point as local builds.

### Available Scripts

| Script | Purpose |
|--------|---------|
| `release_extension.py` | Bump version, create tag, trigger CI |
| `submit_to_registry.py` | Submit release to extension registry |
| `release_tools.py` | Validation and packaging utilities |

**Full documentation:** [`scripts/README.md`](scripts/README.md) — detailed pipeline diagram, CLI reference, troubleshooting.

## Dependencies

### Via Conan

[`plotjuggler_sdk`](https://github.com/PlotJuggler/plotjuggler_sdk) is consumed as a Conan package;
every per-plugin `conanfile.py` lists it too. The version is pinned in the top-level `SDK_VERSION`
file, and CI clones and builds the matching `v<SDK_VERSION>` tag when no prebuilt package is
available (`scripts/ensure_core.sh`).

| Package | Version | Used by |
|---------|---------|---------|
| **plotjuggler_sdk** (cloudsmith) | pinned via `SDK_VERSION` (exact) | **SDK + host loaders** (`plotjuggler_sdk::plugin_sdk`, `::plugin_host`) |
| nlohmann_json | 3.12.0 | Most plugins |
| arrow + parquet | 26.0.1 | data_load_parquet, data_load_lerobot, parser_arrow, toolbox_data_exporter, toolbox_mosaico |
| nanoarrow | 0.9.0 (fetched by `cmake/Nanoarrow.cmake` until ConanCenter has it) | parser_arrow (C Data column shaping) |
| paho-mqtt-cpp | 1.6.0 | data_stream_mqtt |
| cppzmq | 4.11.0 | data_stream_zmq |
| protobuf | 6.33.5 | parser_protobuf |
| zstd | 1.5.7 | data_load_mcap, data_stream_pj_bridge |
| lz4 | 1.10.0 | data_load_mcap |
| date | 3.0.4 | data_load_csv, data_load_mp4, data_load_parquet |
| ffmpeg | 8.1.2 | data_load_lerobot, data_load_mp4 |
| ixwebsocket | 12.0.0 | data_stream_foxglove_bridge, data_stream_pj_bridge, data_stream_webrtc |
| asio | 1.28.2 | data_stream_udp |
| liblsl | 1.16.2 | data_stream_lsl |
| pocketfft | 0.0.0.cci.20240801 | toolbox_fft |
| lua | 5.4.6 | toolbox_mosaico |
| sol2 | 3.5.0 | toolbox_mosaico |
| libdatachannel | 0.24.0 | data_stream_webrtc |
| fmt | 12.1.0 | data_load_mp4, toolbox_mosaico |
| zlib | 1.3.2 | data_load_mf4 (mdflib), data_load_blf |
| expat | 2.10.0 | data_load_mf4 (mdflib) |
| gtest | 1.17.0 | All plugin tests |
| libcurl | 8.23.0 | tools/anomaly_runner (webhook/email notifications) |
| pj_scripting_core | 0.1.0 | tools/anomaly_runner — shared Luau marker engine (carries Luau + kissfft) |

### Via CPM (plugin-private deps only)

| Package | Used by |
|---------|---------|
| ulog_cpp | data_load_ulog |
| rosx_introspection | parser_ros |
| data_tamer | parser_ros, parser_data_tamer |
| mdflib | data_load_mf4 |
| dbc_parser_cpp + fast_float | data_load_mf4, data_load_blf, data_load_candump (shared `common/can_dbc`) |
| lblf | data_load_blf |

### Pinned transitive dependencies

| Package | Version | Reason |
|---------|---------|--------|
| libsodium | 1.0.22 | Not 1.0.21: its ARM NEON code fails with GCC on aarch64 |
| lz4 (root aggregate build only) | 1.9.4 | Must equal arrow/26.0.1's own lz4 pin on Linux (Flight build), or Conan splits Arrow into a second build; data_load_mcap's own standalone recipe still pins lz4/1.10.0 unaffected |
