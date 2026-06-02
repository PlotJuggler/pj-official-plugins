# PlotJuggler Ported Plugins

[![CI Linux](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml)
[![CI Windows](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml)
[![CI macOS](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml)


Plugin collection for PlotJuggler Core: message parsers (JSON, Protobuf, ROS,
DataTamer), file loaders (CSV, MCAP, Parquet, ULog, MP4, LeRobot), streaming
sources (ZMQ, MQTT, UDP, ROS 2, Foxglove and PlotJuggler bridges), and toolbox
widgets (FFT, quaternion, colormap, reactive scripts). See the
[Plugins](#plugins) table for the full list and per-plugin build status.

## Building

### Standalone (requires Conan 2.x)

Configure the plotjuggler cloudsmith Conan remote once per machine so
`plotjuggler_sdk` resolves on `conan install`:

```bash
conan remote add plotjuggler-cloudsmith \
  https://conan.cloudsmith.io/plotjuggler/plotjuggler
conan remote login plotjuggler-cloudsmith <user> -p <api-key>
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

## Dependencies

### Via Conan

`plotjuggler_sdk` is consumed as a Conan package from the
plotjuggler cloudsmith remote — no CPM source clone, no SSH deploy key, no
subdirectory-mode fallback for standalone builds. Every per-plugin
`conanfile.py` also lists it so single-plugin builds resolve it the same way.
The version is pinned in one place — the top-level `SDK_VERSION` file — and CI
builds core from the pinned `extern/plotjuggler_core` submodule when cloudsmith
is unavailable (`scripts/ensure_core.sh`).

> **Repository & package rename:** the SDK source now lives in the
> [**plotjuggler_sdk**](https://github.com/PlotJuggler/plotjuggler_sdk)
> repository (formerly `plotjuggler_core`), and the Conan package and CMake
> targets are renamed to match — recipes require `plotjuggler_sdk/<version>` and
> link `plotjuggler_sdk::plugin_sdk` / `::plugin_host`. The only thing that keeps
> the old name is the submodule mount point, `extern/plotjuggler_core`.

| Package | Version | Used by |
|---------|---------|---------|
| **plotjuggler_sdk** (cloudsmith) | pinned via `SDK_VERSION` (exact) | **SDK + host loaders** (`plotjuggler_sdk::plugin_sdk`, `::plugin_host`) |
| nlohmann_json | 3.12.0 | Most plugins |
| mcap | 2.1.1 | data_load_mcap |
| arrow + parquet | 23.0.1 | data_load_parquet |
| paho-mqtt-cpp | 1.5.3 | data_stream_mqtt |
| cppzmq | 4.11.0 | data_stream_zmq |
| protobuf | 6.33.5 | parser_protobuf |
| zstd | 1.5.5 | data_stream_pj_bridge |
| date | 3.0.4 | data_load_csv |
| ixwebsocket | 11.4.6 | data_stream_foxglove_bridge, data_stream_pj_bridge |
| asio | 1.28.2 | data_stream_udp |
| kissfft | 131.1.0 | toolbox_fft |
| lua | 5.4.6 | toolbox_colormap, toolbox_reactive_scripts_editor |
| sol2 | 3.5.0 | toolbox_colormap, toolbox_reactive_scripts_editor |
| pybind11 | 2.13.6 | toolbox_reactive_scripts_editor |
| cpython | 3.12.7 | toolbox_reactive_scripts_editor |
| gtest | 1.17.0 | All plugin tests |

### Via CPM (plugin-private deps only)

| Package | Used by |
|---------|---------|
| ulog_cpp | data_load_ulog |
| rosx_introspection | parser_ros |
| data_tamer | parser_ros, parser_data_tamer |

### Pinned transitive dependencies

| Package | Version | Reason |
|---------|---------|--------|
| libsodium | 1.0.20 | 1.0.21 has broken ARM NEON code that fails with GCC on aarch64 |

## Plugins

The **Status** badge shows each plugin's released version and turns green when
its latest build on `main` passed CI (red if it failed). It is published from
CI to the `badges` branch; see [Per-plugin status badges](#per-plugin-status-badges).

| Plugin | Type | Description | Status |
|--------|------|-------------|--------|
| parser_json | MessageParser | JSON message parsing | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_json.json)](parser_json/) |
| parser_protobuf | MessageParser | Protobuf message parsing | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_protobuf.json)](parser_protobuf/) |
| parser_ros | MessageParser | ROS 1/2 message parsing | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_ros.json)](parser_ros/) |
| parser_data_tamer | MessageParser | DataTamer schema/snapshot parsing | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/parser_data_tamer.json)](parser_data_tamer/) |
| data_load_csv | DataSource | CSV file loading | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_csv.json)](data_load_csv/) |
| data_load_mcap | DataSource | MCAP file loading | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_mcap.json)](data_load_mcap/) |
| data_load_parquet | DataSource | Parquet file loading | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_parquet.json)](data_load_parquet/) |
| data_load_ulog | DataSource | ULog file loading | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_ulog.json)](data_load_ulog/) |
| data_load_lerobot | DataSource | LeRobot v2.1 dataset loading | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_lerobot.json)](data_load_lerobot/) |
| data_load_mp4 | DataSource | MP4 video loading | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_load_mp4.json)](data_load_mp4/) |
| data_stream_zmq | DataSource | ZeroMQ streaming | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_zmq.json)](data_stream_zmq/) |
| data_stream_mqtt | DataSource | MQTT streaming | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_mqtt.json)](data_stream_mqtt/) |
| data_stream_foxglove_bridge | DataSource | Foxglove WebSocket bridge | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_foxglove_bridge.json)](data_stream_foxglove_bridge/) |
| data_stream_pj_bridge | DataSource | PlotJuggler WebSocket bridge | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_pj_bridge.json)](data_stream_pj_bridge/) |
| data_stream_ros2 | DataSource | ROS 2 topic streaming | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_ros2.json)](data_stream_ros2/) |
| data_stream_udp | DataSource | UDP unicast/multicast streaming | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_udp.json)](data_stream_udp/) |
| data_stream_dummy | DataSource | Synthetic test-signal generator | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_dummy.json)](data_stream_dummy/) |
| toolbox_fft | Toolbox | FFT frequency-domain transform | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_fft.json)](toolbox_fft/) |
| toolbox_quaternion | Toolbox | Quaternion → roll/pitch/yaw | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_quaternion.json)](toolbox_quaternion/) |
| toolbox_colormap | Toolbox | Lua colormap editor | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_colormap.json)](toolbox_colormap/) |
| toolbox_reactive_scripts_editor | Toolbox | Reactive Lua/Python script editor | [![status](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/toolbox_reactive_scripts_editor.json)](toolbox_reactive_scripts_editor/) |

### Per-plugin status badges

Each **Status** badge is a [shields.io endpoint badge](https://shields.io/badges/endpoint-badge)
backed by a small JSON file on the orphan `badges` branch — one
`<plugin>.json` per plugin holding `{ label, message: version, color }`. A
native GitHub Actions badge can only report pass/fail for a whole workflow, so
this scheme is what lets a single badge carry both the version and the build
color.

CI publishes them on pushes to `main`:

- The CI Linux `per-plugin-build` matrix builds each plugin in isolation;
  every leg emits its badge JSON (version from `manifest.json`, color from that
  leg's outcome) as an artifact, and the `update-badges` job commits all of
  them to the `badges` branch in one push.
- `data_stream_ros2` is built only by CI ROS2, so that workflow owns its badge
  and writes it from its own `update-badge` job (color aggregated from the
  distro matrix and the proxy build).

Both writers share a `badges-branch` concurrency group and resync-on-conflict
(see `scripts/publish_badges.sh`), so their pushes never clobber each other.
Badges first appear after the feature's initial run on `main`; until then
shields renders them as `invalid` because the `badges` branch does not yet
exist.

## Plugin architecture

Both plugin families in this repo follow a **declarative** style on top of
the `plotjuggler_sdk` SDK: a DataSource hands the host a deferred byte
fetcher per message, and a MessageParser declares a table of schema
handlers that produce **canonical objects** (`sdk::Image`,
`sdk::PointCloud`, and related builtin types) plus scalar columns. The host
chooses eager vs lazy materialization per message without either plugin
caring.

See [`PLUGIN_DEVELOPMENT.md`](PLUGIN_DEVELOPMENT.md) for the full
developer guide: the canonical-object vocabulary, the DataSource and
MessageParser shapes, end-to-end dispatch flow, authoring checklists,
and pointers to `data_load_mcap` and `parser_ros` as reference
implementations.

## Development Checklist

When adding or changing a plugin:

1. Keep `manifest.json` current; the release tag version must match it.
2. Add or update the plugin's `CMakeLists.txt`.
3. Add any Conan dependencies to the plugin's `conanfile.py`.
4. Add new dependencies to the root `conanfile.py` when full-repository builds need them.
5. Add focused tests in the plugin directory when behavior changes.

## Releasing Extensions

Each plugin is independently versioned and released. The release pipeline builds the tagged plugin on **6 platforms** (Linux x86_64/aarch64, macOS Intel/ARM, Windows x64/ARM64), creates plugin-scoped release notes, and can automatically submit to the extension registry.

### Quick Start (Recommended)

```bash
# One command: bump version, commit, tag, push, build, submit to registry
python3 scripts/release_extension.py foxglove-bridge --bump minor --submit-to-registry
```

This will:
1. Update `manifest.json` with new version
2. Commit and push the change
3. Create annotated tag → triggers CI
4. CI installs that plugin's Conan recipe, builds it on all 6 platforms, and creates a GitHub Release with notes from that plugin's directory
5. Automatically creates a `pj-plugin-registry` PR for the exact version in the triggering tag

### Tag-Only (Manifest Already Updated)

When manifest already has the correct version (e.g., bumped in a previous commit):

```bash
# No --bump or --version: reads version from manifest, creates tag only
python3 scripts/release_extension.py foxglove-bridge --submit-to-registry
```

Useful for batch releases or re-creating tags after cleanup.

### Tag Convention

```
<source_directory>/v<semver>
```

Examples: `data_load_csv/v1.0.6`, `parser_ros/v2.1.0`

The source directory before `/v` controls the CI build scope. A
`data_load_csv/v1.0.6` tag installs `data_load_csv/conanfile.py` and configures
CMake with `-DPJ_BUILD_PLUGIN=data_load_csv`; it does not install or compile
dependencies for unrelated plugins. CI uses the same `build.sh` entry point as
local standalone builds.

### Available Scripts

| Script | Purpose |
|--------|---------|
| `release_extension.py` | Bump version, create tag, trigger CI |
| `submit_to_registry.py` | Submit release to extension registry |
| `release_tools.py` | Validation and packaging utilities |

**Full documentation:** [`scripts/README.md`](scripts/README.md) — detailed pipeline diagram, CLI reference, troubleshooting.
