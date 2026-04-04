# PlotJuggler Ported Plugins

[![CI Linux](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml)
[![CI Windows](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml)
[![CI macOS](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml)


Plugin collection for PlotJuggler Core — CSV, Parquet, ULog, MCAP, JSON,
Protobuf, ROS, ZMQ, MQTT, Foxglove Bridge, and PJ Bridge.

## Building

### Standalone (requires Conan 2.x)

```bash
# Install third-party dependencies
conan install . --output-folder=build --build=missing

# Configure and build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build
```

### As subdirectory of plotjuggler_core

No extra steps — the parent project's build system handles everything:

```bash
cd /path/to/plotjuggler_core
./build.sh
```

## Dependencies

### Via Conan (third-party)

| Package | Version | Used by |
|---------|---------|---------|
| nlohmann_json | 3.12.0 | Most plugins |
| mcap | 2.1.1 | data_load_mcap |
| arrow + parquet | 23.0.1 | data_load_parquet |
| paho-mqtt-cpp | 1.5.3 | data_stream_mqtt |
| cppzmq | 4.11.0 | data_stream_zmq |
| protobuf | 6.33.5 | parser_protobuf |
| zstd | 1.5.5 | data_stream_pj_bridge |
| date | 3.0.4 | data_load_csv |
| ixwebsocket | 11.4.6 | data_stream_foxglove_bridge, data_stream_pj_bridge |
| gtest | 1.17.0 | All plugin tests |

### Via CPM (GitHub-only)

| Package | Used by |
|---------|---------|
| plotjuggler_core | SDK (pj_base, pj_dialog_sdk, pj_message_parser_host) |
| ulog_cpp | data_load_ulog |
| rosx_introspection | parser_ros |
| data_tamer | parser_ros, parser_data_tamer |

### Pinned transitive dependencies

| Package | Version | Reason |
|---------|---------|--------|
| libsodium | 1.0.20 | 1.0.21 has broken ARM NEON code that fails with GCC on aarch64 |

## Plugins

| Plugin | Type | Description |
|--------|------|-------------|
| parser_json | MessageParser | JSON message parsing |
| parser_protobuf | MessageParser | Protobuf message parsing |
| parser_ros | MessageParser | ROS 1/2 message parsing |
| parser_data_tamer | MessageParser | DataTamer schema/snapshot parsing |
| data_load_csv | DataSource | CSV file loading |
| data_load_mcap | DataSource | MCAP file loading |
| data_load_parquet | DataSource | Parquet file loading |
| data_load_ulog | DataSource | ULog file loading |
| data_stream_zmq | DataSource | ZeroMQ streaming |
| data_stream_mqtt | DataSource | MQTT streaming |
| data_stream_foxglove_bridge | DataSource | Foxglove WebSocket bridge |
| data_stream_pj_bridge | DataSource | PlotJuggler WebSocket bridge |

## Releasing Extensions

Each plugin is independently versioned and released. The release pipeline builds on **6 platforms** (Linux x86_64/aarch64, macOS Intel/ARM, Windows x64/ARM64) and can automatically submit to the extension registry.

### Quick Start (Recommended)

```bash
# One command: bump version, commit, tag, push, build, submit to registry
python3 scripts/release_extension.py foxglove-bridge --bump minor --submit-to-registry
```

This will:
1. Update `manifest.json` with new version
2. Commit and push the change
3. Create annotated tag → triggers CI
4. CI builds all 6 platforms and creates GitHub Release
5. Automatically creates PR to `pj-plugin-registry`

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

### Available Scripts

| Script | Purpose |
|--------|---------|
| `release_extension.py` | Bump version, create tag, trigger CI |
| `submit_to_registry.py` | Submit release to extension registry |
| `release_tools.py` | Validation and packaging utilities |

**Full documentation:** [`scripts/README.md`](scripts/README.md) — detailed pipeline diagram, CLI reference, troubleshooting.
