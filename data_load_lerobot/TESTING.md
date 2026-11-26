# Manual testing the LeRobot loader

Run from the repository root.

## Build and unit tests

```bash
./build.sh data_load_lerobot
ctest --test-dir build/data_load_lerobot/Release -R lerobot --output-on-failure
```

The five test binaries cover `meta/info.json` / `episodes.jsonl` / `tasks.jsonl` parsing, vector-column naming and
dedupe, the dialog's `__pj_fanout` serialization, the v3.0 shard reader, and the v2.x / v3.0 video window
resolution (keyframe seek-back, presentation window, rebase). `pj_video_demux_test` covers the shared demuxer
(h264 / h265 / av1 index and Annex-B / OBU rewrite).

The build produces `liblerobot_source_plugin.so` and the `lerobot_source_plugin.pjmanifest.json` sidecar in
`build/data_load_lerobot/Release/bin/`. Both go into the PlotJuggler 4 extensions folder; the repo's
`manifest.json` is build input only and is not installed.

```bash
DST=~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader
mkdir -p "$DST"
cp build/data_load_lerobot/Release/bin/liblerobot_source_plugin.so \
   build/data_load_lerobot/Release/bin/lerobot_source_plugin.pjmanifest.json "$DST/"
```

## Sample datasets

`lerobot/pusht` on `main` is already **v3.0**; request the `v2.1` tag for a v2.x dataset (`huggingface-cli` is
deprecated, use `huggingface_hub`):

```bash
python3 -c "
import os
from huggingface_hub import snapshot_download
snapshot_download('lerobot/pusht', repo_type='dataset', revision='v2.1',
  local_dir=os.environ['LEROBOT_DATASET_DIR'] + '/pusht_v21',
  allow_patterns=['meta/*','data/chunk-000/episode_000000.parquet',
                  'videos/chunk-000/*/episode_000000.mp4'])
"
```

| Version | Hub repo | Size | Episodes | Notes |
|---|---|---|---|---|
| v3.0 | `lerobot/example_hil_serl_dataset` | 1.3 MB | 15 | 1 camera (`observation.images.top`), av1, 10 FPS; ideal for iteration |
| v3.0 | `lerobot/svla_so100_sorting` | 832 MB | 52 | 2 cameras, 30 FPS |
| v2.1 | `lerobot/svla_so101_pickplace` | 86 MB | 50 | 2 cameras, 30 FPS |
| v2.0 | `lerobot/pusht` | 7.7 MB | 206 | 1 camera, 10 FPS |

## Checks in PlotJuggler 4

The PJ4 build needs `FileVideoSource` wired into `Media2DDockWidget` and libdav1d in the host's FFmpeg (AV1 video),
otherwise camera topics do not render. The plugin itself decodes nothing.

1. **File -> Open**, filter `*.json`, pick the dataset's `meta/info.json`.
2. The **LeRobot Dataset** dialog shows path, version, fps, episode count and cameras, plus the episode list
   (`ep N · L frames · task`). Multi-select works; **OK** stays disabled with nothing selected.
3. Scalar series appear under topic `lerobot` with names from `info.json` (pusht v2.1:
   `lerobot/observation.state.motor_0`, `lerobot/action.motor_0`, `lerobot/next.reward`, ...); dragging one onto a
   plot gives a continuous curve.
4. Each selected episode is its own dataset (`pusht_v21/ep_3`, ...), not concatenated into one timeline.
5. Each camera is an object topic under its episode (`lerobot/observation.image`). Drag it onto an empty 2D view;
   the frame follows the time cursor. Multiple cameras need one 2D view each.
6. v3.0: scrubbing past the episode end clamps to its clip window. v2.x: it stops at the file's last frame.

## Troubleshooting

- A camera shows in the tree but nothing renders: the PJ4 build lacks libdav1d (AV1) or the codec support for that
  stream.
- The dialog does not appear for `info.json`: check that the extension folder contains the `.pjmanifest.json`
  sidecar with `"file_extensions": [".json"]`.
