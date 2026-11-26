# LeRobot Data Loader

Imports [LeRobot](https://github.com/huggingface/lerobot) robotics datasets (**v2.0 / v2.1 and v3.0**). The user
picks the dataset's `meta/info.json`; the plugin detects the format from `codebase_version`, walks up to confirm the
dataset root, lets the user multi-select episodes, and **spawns one plugin instance per selected episode**. Each
instance gets its own `DatasetId`, so an episode is also a dataset in the PlotJuggler catalog (each with its own
clock starting at 0).

The plugin registers only the `.json` extension so it does not shadow `data_load_parquet` for plain parquet files.

## What gets imported

| Source | Goes to | How |
|---|---|---|
| Per-frame scalar columns from the data parquet | `DataEngine` topic `lerobot` | One field per Arrow column, native types preserved. Per-row timestamp from the parquet `timestamp` column, else `frame_index / fps`. |
| `list<float>` / `fixed_size_list<float>` columns (`observation.state`, `action`, ...) | `DataEngine` topic `lerobot`, one field per element | Named `<col>.<name>` from `info.json`'s `features[...].names` when present, otherwise `<col>.<i>`; duplicates across columns get a `__2`, `__3`, ... suffix. |
| MP4 per camera | `ObjectStore` topic `lerobot/<cam>`, schema `PJ.VideoFrame` | **Lazy per frame.** The MP4 is demux-indexed once (no decode) via `pj_video_demux`; each access unit is pushed as a lazy `PJ.VideoFrame` whose bytes are read from the file on demand, so the video never lands on the heap. H.264 / H.265 / AV1. The host's streaming video decoder drives playback. |

The plugin does not decode video. `pj_video_demux` links FFmpeg only to walk the container.

## v2.x vs v3.0

| | v2.x | v3.0 |
|---|---|---|
| Episode metadata | `meta/episodes.jsonl` | `meta/episodes/chunk-XXX/file-NNN.parquet` (relational: `dataset_from_index`, `data/chunk_index`, `videos/{cam}/from_timestamp`, ...) |
| Data files | `data/chunk-XXX/episode_NNNNNN.parquet` (one per episode) | `data/chunk-XXX/file-NNN.parquet` (a shard holds many episodes; the episode's rows `[dataset_from_index, dataset_to_index)` are sliced out) |
| Video files | `videos/chunk-XXX/<cam>/episode_NNNNNN.mp4` | `videos/<cam>/chunk-XXX/file-NNN.mp4` (one shard per chunk and camera) |
| Path templates | `{episode_chunk:03d}` / `{episode_index:06d}` | `{chunk_index:03d}` / `{file_index:03d}` |
| Video emitted | the whole file, rebased to its first DTS | the episode's presentation window `[start_ns, end_ns)` of the shared MP4, starting at the keyframe at-or-before `start_ns` and rebased so `start_ns` is episode-local 0; scrubbing past the episode end never reveals the next episode's frames |

`gateVersion` accepts `v2.0` / `v2.1` and any `v3.x`. Both layouts are normalized into the same
`episode_shards` map, so the importer has no per-version branching (v2.x is the case `row_from = 0`,
`row_to = length`, no video window).

## Multi-episode imports (`__pj_fanout`)

With N episodes selected, the dialog's `saveConfig()` emits a top-level `__pj_fanout` array with one per-instance
config per episode; the host runs one import per entry, each against its own `DatasetId`. The catalog name is
`<dataset-folder>/<display_suffix>`, e.g. `pusht_v21/ep_3`.

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

`selected_episodes` is saved with `selected_episodes_dataset`, the dataset's folder name, and applies to that
dataset only: PlotJuggler pre-fills the dialog of the next dataset with the last config, and a dataset with another
name opens with every episode selected instead.

## Layout

- `lerobot_plugin.cpp`: `LeRobotSource::importData()` (column plan, parquet scan, one lazy video topic per camera).
- `lerobot_dialog.hpp`: Qt-free dialog state, episode multi-select, config save/load and `__pj_fanout` emission.
- `dataset_model.{hpp,cpp}`: parses `meta/info.json` and the v2.x metadata; builds the version-agnostic
  `episode_shards` map. No Arrow.
- `dataset_shards_v3.{hpp,cpp}`: Arrow-backed reader for the v3.0 `meta/episodes` parquet.
- `flatten_plan.{hpp,cpp}`: names for flattened vector columns.
- `lerobot_video_window.{hpp,cpp}`: which video access units an episode emits and the rebase origin.

## Build and test

```bash
./build.sh data_load_lerobot
ctest --test-dir build/data_load_lerobot/Release -R lerobot --output-on-failure
```

`TESTING.md` walks through installing the plugin into PlotJuggler 4 and checking it against a real dataset.

## Known limitations

- The dataset path is not restored across sessions (the user picks the file each time); the episode selection is.
- v3 `stats/*` columns in `meta/episodes/*.parquet` are skipped; only the columns the importer needs are read.
