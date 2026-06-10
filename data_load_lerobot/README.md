# LeRobot Data Loader

Imports [LeRobot](https://github.com/huggingface/lerobot) robotics datasets
into PlotJuggler. **Supports both v2.x (v2.0 / v2.1) and v3.0**: the user
picks the dataset's `meta/info.json`, the plugin detects the format from
`codebase_version`, and the rest of the import pipeline (schema discovery,
row slicing, per-camera video emission) is layout-blind.

A v2.x dataset pairs one Parquet per episode with one MP4 per (episode, camera),
plus JSONL metadata under `meta/`. A v3.0 dataset reorganizes the same content
into **shard files** that hold many episodes back-to-back (one consolidated
Parquet per chunk, one MP4 per camera per chunk) with **relational metadata**
in `meta/episodes/chunk-XXX/file-NNN.parquet` that maps each episode to its
row range inside the data shard and its `[from_timestamp, to_timestamp]`
inside the video shard.

The user picks `meta/info.json` (the plugin's only registered file extension
is `.json`, so it doesn't shadow `data_load_parquet` for plain parquet files).
From there it walks up to confirm the dataset root, lets the user multi-select
episodes in a dialog, and **spawns one plugin instance per selected episode**.
Each instance gets its own `DatasetId`, so an episode is also a dataset in
the PlotJuggler catalog.

## What gets imported

| Source | Goes to | How |
|---|---|---|
| Per-frame scalar columns from the data parquet | `DataEngine` topic `lerobot` | One field per Arrow column, native types preserved via `ValueRef`. Per-row timestamp from the parquet `timestamp` column or `frame_index / fps`. v2.x reads the entire per-episode parquet; v3.0 slices `[dataset_from_index, dataset_to_index)` out of the consolidated shard parquet. |
| `list<float>` / `fixed_size_list<float>` columns (`observation.state`, `action`, …) | `DataEngine` topic `lerobot`, one field per element | Flattened with names from `info.json`'s `features[...].names` when present; otherwise `<col>_0`, `<col>_1`, …. Dedupe handles cross-column collisions. |
| MP4 file per camera | `ObjectStore` topic `lerobot/<cam>`, schema `PJ.VideoFrame` | **Lazy per-frame.** The MP4 is demux-indexed once (no decode) via the shared `pj_video_demux` helper; each access unit is pushed as a lazy `PJ.VideoFrame` whose compressed bytes are read from the file on demand — the whole video never lands on the heap. Codecs: H.264 / H.265 / AV1. v2.x emits the whole file (one MP4 = one episode, rebased to its first DTS). v3.0 emits the episode's presentation window `[start_ns, end_ns)` inside the shared MP4, seeking back to the keyframe at-or-before `start_ns` and rebasing so episode-local 0 lands on the window start. The host's streaming video decoder drives playback from the per-frame entries. |

## Version handling

| Code path | v2.x | v3.0 |
|---|---|---|
| `gateVersion` (in `dataset_model.cpp`) | accepts `v2.0` / `v2.1` → `DatasetVersion::V2_x` | accepts any `v3.x` → `DatasetVersion::V3_0` |
| Episode metadata | `meta/episodes.jsonl` (flat JSONL, one line per ep) | `meta/episodes/chunk-XXX/file-NNN.parquet` (chunked Parquet with relational columns: `dataset_from_index`, `data/chunk_index`, `videos/{cam}/from_timestamp`, …) |
| Data files | `data/chunk-XXX/episode_NNNNNN.parquet` (one per ep) | `data/chunk-XXX/file-NNN.parquet` (one shard holds many eps) |
| Video files | `videos/chunk-XXX/<cam>/episode_NNNNNN.mp4` (one per (ep, cam)) | `videos/<cam>/chunk-XXX/file-NNN.mp4` (one shard per (chunk, cam)) |
| Path templates in `info.json` | `{episode_chunk:03d}` / `{episode_index:06d}` placeholders | `{chunk_index:03d}` / `{file_index:03d}` placeholders |
| Shard reader | `parseEpisodes` + `synthesizeShardsV2` in `dataset_model.cpp` | `loadV3EpisodeShards` in `dataset_shards_v3.cpp` (Arrow-backed) |
| Result | `episode_shards[ep]` with `row_from=0, row_to=length, videos[cam].start_ns/end_ns=nullopt` | `episode_shards[ep]` with shard's `(row_from, row_to)` + per-camera `(start_ns, end_ns)` populated from `from_timestamp * 1e9` |

The importer (`importEpisode` + `importVideoAssets` in `lerobot_plugin.cpp`)
consumes `episode_shards` directly, so v2.x is a special case of v3.0 where
the row range is `[0, length)` and the clip-window optionals are absent.
There is **no per-version branching** in the row loop or the video
registration.

## Multi-episode imports — `__pj_fanout`

When the user accepts the dialog with N episodes selected, the dialog's
`saveConfig()` emits a top-level `__pj_fanout` array with one per-instance
config per episode:

```json
{
  "filepath": "/datasets/pusht_v21",
  "selected_episodes": [3, 5, 8],
  "__pj_fanout": [
    "{\"filepath\":\"/datasets/pusht_v21\",\"episode\":3,\"display_suffix\":\"ep_3\"}",
    "{\"filepath\":\"/datasets/pusht_v21\",\"episode\":5,\"display_suffix\":\"ep_5\"}",
    "{\"filepath\":\"/datasets/pusht_v21\",\"episode\":8,\"display_suffix\":\"ep_8\"}"
  ]
}
```

`pj_app::FileLoader` peels `__pj_fanout` and runs the import N times — one
fresh `DataSourceHandle` + `DatasetId` per entry. Each spawned
`LeRobotSource` reads `episode: <int>` from its per-instance config and
imports that one episode against its own `DatasetId`. The display name in
the catalog ends up as `<dataset-basename>/<display_suffix>`, e.g.
`pusht_v21/ep_3`.

Single-episode selections and back-compat configs (without `__pj_fanout`)
go through the same `extractFanout` helper in `FileLoader` — it returns a
single-element list, so the host runs one import as before.

## Architecture — what each file does

```
data_load_lerobot/
├── manifest.json          plugin id / name / version / file_extensions
├── conanfile.py           Conan deps: arrow + parquet + nlohmann_json + gtest
├── CMakeLists.txt         shared-library target + 4 unit tests
├── dialog_lerobot.ui      Qt Designer .ui — embedded at build time as a const char[]
│
├── lerobot_plugin.cpp     LeRobotSource: importData() entry point. Reads the
│                          single episode index from the dialog, builds the
│                          column plan, runs the parquet scan, registers one
│                          metadata-only video topic per camera.
├── lerobot_dialog.hpp     LeRobotDialog: Qt-free dialog state. Owns the
│                          DatasetModel, the multi-select episode list, and
│                          the save/load JSON (including __pj_fanout emission).
│
├── dataset_model.{hpp,cpp}      Parses meta/info.json + meta/episodes.jsonl
│                                 (v2.x) into a DatasetModel struct. Pure, no
│                                 Arrow, no host APIs. Synthesizes the
│                                 version-agnostic episode_shards map for v2.x.
├── dataset_shards_v3.{hpp,cpp}  Arrow-backed reader for v3.0
│                                 meta/episodes/chunk-XXX/file-NNN.parquet.
│                                 Populates the same episode_shards map the
│                                 importer consumes — no Arrow needed in
│                                 dataset_model.cpp.
├── flatten_plan.{hpp,cpp}       Flatten vector columns into per-element field
│                                 names with dedupe. Pure, testable.
│   (Arrow scalar/vector cell extraction → PJ::sdk ValueRef comes from the
│    shared common/arrow_helpers module, included as pj_arrow_helpers.)
│
└── tests/                    GTest binaries
    ├── dataset_model_test.cpp       v2.x fixture (synthesized parquet stubs)
    ├── dataset_model_v3_test.cpp    v3.0 fixture (real Arrow-written parquets)
    ├── flatten_plan_test.cpp
    ├── dialog_fanout_test.cpp
    └── video_window_test.cpp        v2.x/v3.0 emit-slice resolution (pure)
```

The plugin does **not** decode video. It demux-indexes each MP4 (via the shared
`pj_video_demux` helper, which links FFmpeg only to walk the container — no
decode, no bitstream filters) and pushes lazy `PJ.VideoFrame` entries; the host's
streaming video decoder decodes them on demand.

## How a DataSource plugin gets integrated — the short version

Reading `lerobot_plugin.cpp` end-to-end is the fastest introduction to the
v4 plugin ABI; the file is short and self-contained:

1. **Subclass `PJ::FileSourceBase`** and implement `importData()` —
   one virtual call. The base class handles state machine, `requestStop`,
   progress finish, etc.
2. **Declare a config dialog** (optional) by subclassing
   `PJ::DialogPluginTyped` and storing one as a member of the source, then
   override `getDialog()` to return a borrowed pointer.
3. **Register topics** via `writeHost()` (scalars) and `objectWriteHost()`
   (binary blobs / media). Topics carry a `metadata_json` string built by
   `PJ::sdk::MediaMetadataBuilder` — the host reads it to pick a renderer
   and (for our case) to discover the video file path.
4. **Export** with `PJ_DATA_SOURCE_PLUGIN(LeRobotSource, kLerobotManifest)`
   and `PJ_DIALOG_PLUGIN(LeRobotDialog)` at file scope.
5. **Multi-instance**: if the dialog confirms a selection that should
   produce multiple datasets, emit a `__pj_fanout` array of sub-config JSON
   strings from `saveConfig()`. `pj_app::FileLoader` does the rest.

The whole ABI surface that this plugin touches lives under `pj_base/include/pj_base/`
and `pj_plugins/sdk/`; see `PLUGIN_DEVELOPMENT.md` at the repo root for the
broader picture.

## Build

```bash
./build.sh data_load_lerobot
```

Outputs `liblerobot_source_plugin.so` into
`build/data_load_lerobot/Release/bin/` alongside `lerobot_source_plugin.pjmanifest.json`
that the host scans for plugin discovery.

## Smoke testing — try it against real datasets

Unit tests cover both versions end-to-end (`lerobot_dataset_model_test` for
v2.x, `lerobot_dataset_model_v3_test` for v3.0 with a real Arrow-written
fixture). To validate the full UI pipeline visually, launch `pj_app` with
this plugin and load a Hub dataset:

```bash
# 1) Build the plugin (Conan + CMake; assumes the local plotjuggler_core build
#    is already published into the Conan cache as plotjuggler_core/0.4.1-clipwindow
#    until the upstream patch release ships).
conan install . --output-folder=build_integrated --build=missing \
    -s compiler.cppstd=20 -s build_type=Release
cmake --preset=conan-release
cmake --build build_integrated -j$(nproc) --target lerobot_source_plugin

# 2) Run all the lerobot unit tests:
ctest --test-dir build_integrated -R lerobot --output-on-failure

# 3) Launch pj_app pointing at the plugin's bin/ output:
/path/to/PJ4/build/pj_app/pj_app \
    --plugin-dir $(pwd)/build_integrated/bin/

# 4) In the UI: File → Open → pick the dataset's meta/info.json
```

Suggested reference datasets (any one works for a smoke pass):

| Version | Dataset (HuggingFace Hub repo) | Size | Episodes | Notes |
|---|---|---|---|---|
| **v3.0** | `lerobot/example_hil_serl_dataset` | 1.3 MB | 15 | Tiny; 1 camera (`observation.images.top`), av1 codec, 10 FPS. Ideal for iteration. |
| **v3.0** | `lerobot/svla_so100_sorting` | 832 MB | 52 | 2 cameras at 30 FPS, so100 robot. More realistic. |
| **v2.1** | `lerobot/svla_so101_pickplace` | 86 MB | 50 | 2 cameras at 30 FPS, so100_follower robot. |
| **v2.0** | `lerobot/pusht` | 7.7 MB | 206 | Classic small dataset, 1 camera, 10 FPS. |

Download example (only the smallest pieces of a dataset):

```bash
hf download lerobot/example_hil_serl_dataset --repo-type=dataset \
    --local-dir ~/bags/datasets/lerobot_example_hil_serl_dataset
```

What to check in the UI:

- **Numeric series** appear under topic `lerobot` with one curve per scalar
  feature (action components, observation.state, next.reward, …).
- **Video panel** opens for each camera (e.g. `lerobot/observation.images.top`)
  and the playhead syncs with the global tracker.
- For v3.0: scrubbing past the episode end **clamps to the clip window** —
  the video never reveals frames from the next concatenated episode.
- For v2.x: scrubbing past the file's last frame stops at the end (same as
  any single-clip MP4).
- Multi-episode selection: pick N episodes in the dialog → N `DatasetId`s
  appear in the catalog (one fanout entry per episode), each with its own
  numeric and video topics.

## Known limitations

- Episode selection state is restored across sessions, but the dataset path is
  not (the user picks the file each time).
- `info.json` and any `.parquet` inside the dataset both work as the entry
  point; `dataset_model::resolveRoot` walks up to find `meta/info.json`.
- v3 datasets that ship `stats/*` columns in `meta/episodes/*.parquet` are
  read tolerantly — those columns are skipped, the importer reads only the
  ones it needs (`dataset_from_index`, `videos/{cam}/from_timestamp`, …).
