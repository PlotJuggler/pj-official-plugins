# LeRobot Data Loader

Imports [LeRobot v2.1](https://github.com/huggingface/lerobot) robotics datasets
into PlotJuggler. A LeRobot dataset is a directory layout that pairs Parquet
files (per-frame numeric data — state, action, rewards, …) with one MP4 per
camera per episode, plus a `meta/info.json` describing the schema and an
`episodes.jsonl` describing the episodes.

The user picks the dataset's `meta/info.json` (the plugin's only registered
file extension is `.json`, so it doesn't shadow `data_load_parquet` for plain
parquet files). From there it walks up to confirm the dataset root, lets the
user multi-select episodes in a dialog, and **spawns one plugin instance per
selected episode**. Each instance gets its own `DatasetId`, so an episode is
also a dataset in the PlotJuggler catalog.

## What gets imported

| Source | Goes to | How |
|---|---|---|
| `data/chunk-NNN/episode_NNNNNN.parquet` scalar columns | `DataEngine` topic `lerobot` | One field per Arrow column, native types preserved via `ValueRef`. Per-row timestamp from the parquet `timestamp` column or `frame_index / fps`. |
| `data/...` `list<float>` / `fixed_size_list<float>` columns (`observation.state`, `action`, …) | `DataEngine` topic `lerobot`, one field per element | Flattened with names from `info.json`'s `features[...].names` when present; otherwise `<col>_0`, `<col>_1`, …. Dedupe handles cross-column collisions. |
| `videos/chunk-NNN/<cam>/episode_NNNNNN.mp4` | `ObjectStore` topic `lerobot/<cam>` | **Metadata-only.** No bytes are pushed. The topic carries `video_file_path` pointing at the absolute MP4 path; the host's `Media2DDockWidget` opens a `FileVideoSource` on that file with FFmpeg's lazy seek + ThumbnailCache. |

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
├── CMakeLists.txt         shared-library target + 3 unit tests
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
├── dataset_model.{hpp,cpp}   Parses meta/info.json + episodes.jsonl into a
│                              DatasetModel struct. Pure, no host APIs.
├── flatten_plan.{hpp,cpp}    Flatten vector columns into per-element field
│                              names with dedupe. Pure, testable.
├── lerobot_arrow_helpers.hpp Arrow scalar/vector cell extraction → PJ::sdk
│                              ValueRef. Mirrors data_load_parquet's helpers.
│
└── tests/                 GTest binaries, one per .{hpp,cpp} pair worth pinning
    ├── dataset_model_test.cpp
    ├── flatten_plan_test.cpp
    └── dialog_fanout_test.cpp
```

The plugin does **not** decode video — that responsibility moved to the host
via [PJ4's `FileVideoSource`](https://github.com/PlotJuggler/PJ4) when the
metadata-only model landed. FFmpeg is no longer a dependency here.

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

## Known limitations

- LeRobot **v2.1 only** — earlier versions used different metadata schemas.
- Episode selection state is restored across sessions, but the dataset path is
  not (the user picks the file each time).
- `info.json` and any `.parquet` inside the dataset both work as the entry
  point; `dataset_model::resolveRoot` walks up to find `meta/info.json`.
