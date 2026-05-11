# PlotJuggler Ported Plugins

[![CI Linux](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-linux.yml)
[![CI Windows](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-windows.yml)
[![CI macOS](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/PlotJuggler/pj-official-plugins/actions/workflows/ci-macos.yml)


Plugin collection for PlotJuggler Core — CSV, Parquet, ULog, MCAP, JSON,
Protobuf, ROS, ZMQ, MQTT, Foxglove Bridge, and PJ Bridge.

## Building

### Standalone (requires Conan 2.x)

```bash
# Build the full plugin collection
./build.sh
```

To work on only one plugin, pass the plugin directory:

```bash
./build.sh data_load_csv
```

Run `./build.sh --help` to see the available arguments.

By default, `build.sh` installs the root `conanfile.txt` and builds into
`build/all/Release`. With a plugin argument, it installs that plugin's
`conanfile.py`, configures CMake with `-DPJ_BUILD_PLUGIN=<plugin_dir>`, and
builds into `build/<plugin_dir>/Release`.

Each plugin directory has its own `conanfile.py`. Keep it in sync with the
plugin's `find_package(... REQUIRED)` dependencies in `CMakeLists.txt`. The
root `conanfile.txt` remains the full-repository dependency set for local full
builds and scheduled CI.

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
| asio | 1.28.2 | data_stream_udp |
| kissfft | 131.1.0 | toolbox_fft |
| lua | 5.4.6 | toolbox_colormap, toolbox_reactive_scripts_editor |
| sol2 | 3.5.0 | toolbox_colormap, toolbox_reactive_scripts_editor |
| pybind11 | 2.13.6 | toolbox_reactive_scripts_editor |
| cpython | 3.12.7 | toolbox_reactive_scripts_editor |
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

## Plugin architecture — the declarative shape

Both plugin families in this repo follow a **declarative** style on top of the
`plotjuggler_core` SDK: instead of writing imperative dispatch logic, you
declare a small piece of metadata (a closure, a schema table) and let the
host route everything. This lets the host pick its own ingest strategy
(eager vs lazy, with-objects vs scalar-only) without the plugin caring.

### DataSource plugins — `pushMessage` with a deferred byte fetcher

A DataSource doesn't deliver bytes; it delivers a **callable that produces
bytes when invoked**. One call per message, regardless of policy:

```cpp
runtimeHost().pushMessage(
    binding_handle, timestamp_ns,
    [reader = reader_shared_ptr_, offset = msg.offset]() -> PJ::sdk::PayloadView {
      // Materialize the bytes on demand. Idempotent — the host may invoke
      // this zero, one, or many times depending on the active policy and
      // consumer pulls.
      return readMessageBytesAt(reader, offset);
    });
```

The closure can return:

- `PJ::sdk::PayloadView { bytes, anchor }` — preferred. Zero-copy
  `Span<const uint8_t>` over a buffer the plugin keeps alive via the
  `BufferAnchor` (typically a `shared_ptr<vector<uint8_t>>` referencing the
  source's chunk/page).
- `std::vector<uint8_t>` — legacy. The SDK template heap-allocates the
  vector and treats it as its own anchor.

What the DataSource **does NOT** do:

- Doesn't consult ingest policy. The host applies `kEager` / `kLazyObjectsEagerScalars` / `kPureLazy` per-message via an `ObjectIngestPolicyResolver`.
- Doesn't invoke the parser. The host does, when and only when it's the right time.
- Doesn't push to the ObjectStore directly. The host orchestrates.

This shape lets the same `pushMessage` call result in either an immediate
parse + store, or a deferred entry that the host materializes only when a
consumer pulls — the plugin is the same. `data_load_mcap` is the reference
implementation; see its README.

### MessageParser plugins — `SchemaHandler` table

A parser doesn't override `parse()`; it declares a **table of handlers**,
one entry per schema type name it knows how to translate:

```cpp
// In your plugin's class scope: a static catalog of schemas. Pure data —
// member-function pointers, no `this` capture.
const auto& MyParser::catalog() {
  using Kind = PJ::sdk::CanonicalObjectKind;
  static const std::unordered_map<std::string, CatalogEntry> kMap = {
      // Canonical-object schema: produces an sdk::Image / CompressedImage /
      // PointCloud via parse_object, plus small-metadata scalars via the
      // canonical scalar route.
      {"my_pkg/MyImage",
          {.object_kind   = Kind::kImage,
           .parse_scalars = &MyParser::imageScalars,
           .parse_object  = &MyParser::parseImage}},

      // Scalar-only schema: just emits columns.
      {"my_pkg/MyTelemetry",
          {.parse_scalars = &MyParser::telemetryScalars}},
  };
  return kMap;
}

// In bindSchema, look up the bound type and register the single handler
// this instance needs (specific from the catalog, or a default fallback).
PJ::Status MyParser::bindSchema(std::string_view type_name,
                                 PJ::Span<const uint8_t> schema) {
  base::bindSchema(type_name, schema);
  // ... parser-specific setup ...

  auto it = catalog().find(canonical_name(type_name));
  // ... build a SchemaHandler from the entry and registerSchemaHandler ...
}
```

The base class implements `classifySchema` / `parseScalars` / `parseObject`
as table lookups. There is no enum to maintain, no switch to extend, and no
virtual override surface — adding a schema is a new entry in the catalog
and the corresponding member-function.

Because the dispatch is declarative, the host (PJ4's runtime) can choose
how to call into the parser per message:

- **Eager** — invoke `parseScalars` + `parseObject` immediately, materialize
  the canonical bytes in the `ObjectStore`.
- **Lazy objects, eager scalars** — invoke `parseScalars` now for columns,
  retain the fetcher for later `parseObject` calls on consumer pulls.
- **Pure lazy** — register the fetcher in the `ObjectStore` and don't call
  the parser at all until something pulls.

The parser doesn't know which mode is active. It just answers questions
honestly when asked.

### How it fits together end-to-end

```
DataSource          Host (PJ4)                MessageParser         ObjectStore
─────────           ──────────                ─────────────         ───────────
pushMessage(ts,
   fetcher)   ──►   resolver.resolve(...)
                      ▼
                    kEager ─► fetcher.fetch()
                              parser.parseScalars  ─►  schemaHandler.parse_scalars
                              parser.parseObject   ─►  schemaHandler.parse_object
                              pushOwned(bytes)                              ─► [bytes]

                    kLazyObjectsEagerScalars ─► fetcher.fetch()
                                                parser.parseScalars  ─►  schemaHandler.parse_scalars
                                                pushLazy(fetcher_closure)              ─► [fetcher]
                                                                  (pull later)         ─► fetcher.fetch + parseObject

                    kPureLazy ─► pushLazy(fetcher_closure)                              ─► [fetcher]
                                                                  (pull later)         ─► fetcher.fetch + parseObject
```

`parser_ros` (canonical-object handlers for Image/CompressedImage/PointCloud2
+ specialized scalar handlers for Imu, JointState, Pose, …) and
`data_load_mcap` (deferred fetcher closure capturing the open `McapReader`)
are the canonical reference implementations in this collection.

## Development Checklist

When adding or changing a plugin:

1. Keep `manifest.json` current; the release tag version must match it.
2. Add or update the plugin's `CMakeLists.txt`.
3. Add any Conan dependencies to the plugin's `conanfile.py`.
4. Add new dependencies to the root `conanfile.txt` when full-repository builds need them.
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
