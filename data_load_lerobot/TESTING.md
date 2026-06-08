# Manual testing guide for the LeRobot Loader plugin

Step-by-step procedure to validate the plugin from scratch. Assumes:

- Plugin repo at `~/Work/pj-official-plugins` (branch `feature/data_load_lerobot`).
- Local `plotjuggler_core` at `~/Work/PJ4/plotjuggler_core`.
- PlotJuggler 4 built under `~/Work/PJ4` from the `feature/file-backed-video-lerobot`
  branch (this is the branch that wires `FileVideoSource` into
  `Media2DDockWidget` and enables libdav1d in the host's FFmpeg — without it,
  camera topics will not render).
- Extensions folder at `~/.local/share/PlotJuggler/PlotJuggler4/extensions/`.
- `conan` 2.x, `cmake`, `ninja` and `huggingface_hub` (Python) available.

---

## 1. Build the plugin (`.so`)

The plugin only needs Arrow + Parquet + nlohmann_json (no FFmpeg — the plugin
no longer decodes video, the host does via `FileVideoSource`). `build.sh` is
not usable here because it clones `plotjuggler_core` over SSH and doesn't
forward extra arguments; we invoke cmake by hand, pointing CPM at the
**local** `plotjuggler_core`.

```bash
cd ~/Work/pj-official-plugins
BUILD=build/data_load_lerobot/Release

conan install data_load_lerobot --output-folder="build/data_load_lerobot" \
  --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja

cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/data_load_lerobot/conan_toolchain.cmake" \
  -DCMAKE_PREFIX_PATH="$PWD/build/data_load_lerobot" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPJ_BUILD_PLUGIN=data_load_lerobot \
  -DCPM_plotjuggler_core_SOURCE=$HOME/Work/PJ4/plotjuggler_core

cmake --build "$BUILD" --parallel
```

**Expected:** `liblerobot_source_plugin.so` (~ 18 MB) plus the
`lerobot_source_plugin.pjmanifest.json` sidecar in
`build/data_load_lerobot/Release/bin/`.

> First build: conan builds Arrow from source (several minutes).
> Subsequent builds: cached, seconds.

---

## 2. Unit tests

Five GTest binaries cover what's headless-verifiable: parsing `meta/info.json`
plus `episodes.jsonl` plus `tasks.jsonl`, flattening and deduping vector-column
names, the dialog's `__pj_fanout` serialization, and the v2.x/v3.0 video
emit-slice resolution (`video_window_test` — keyframe-seek-back + presentation
window + rebase). The shared `pj_video_demux` helper additionally ships
`pj_video_demux_test` (h264/h265/av1 index + Annex-B/OBU rewrite against
committed fixtures).

```bash
ctest --test-dir build/data_load_lerobot/Release -R lerobot --output-on-failure
```

**Expected:** `100% tests passed, 0 tests failed out of 5`.

---

## 3. Install the plugin into PlotJuggler 4

Only two artifacts go into the extensions folder: the `.so` and the
`.pjmanifest.json` sidecar. The repo's `manifest.json` is `cmake` **input**
(embedded into the `.so` and transformed into the sidecar); it is not
installed.

```bash
DST=~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader
mkdir -p "$DST"
SRC=~/Work/pj-official-plugins/build/data_load_lerobot/Release/bin
cp "$SRC/liblerobot_source_plugin.so" "$DST/"
cp "$SRC/lerobot_source_plugin.pjmanifest.json" "$DST/"
ls "$DST"
```

It should sit next to `parquet-loader/`, `mcap-loader/`, etc.

---

## 4. Grab a sample LeRobot v2.1 dataset

⚠️ `lerobot/pusht` on `main` is **already v3.0** (the plugin will reject it
with a clear message — that's the correct behaviour). You have to request
the **`v2.1` tag**. `huggingface-cli` is deprecated; use `huggingface_hub`
from Python:

```bash
python3 -c "
from huggingface_hub import snapshot_download
snapshot_download('lerobot/pusht', repo_type='dataset', revision='v2.1',
  local_dir='$HOME/datasets/pusht_v21',
  allow_patterns=['meta/*','data/chunk-000/episode_000000.parquet',
                  'videos/chunk-000/*/episode_000000.mp4'])
"
grep -o '\"codebase_version\"[^,]*' ~/datasets/pusht_v21/meta/info.json  # → "v2.1"
ls ~/datasets/pusht_v21/data/chunk-000
find ~/datasets/pusht_v21/videos -name '*.mp4'
```

(Drop `allow_patterns` to fetch the full dataset — it's still small. If you
want more episodes, add their `episode_0000NN.parquet` / `.mp4` files.)

> The pusht v2.1 video stream is **AV1**. PJ4 decodes it with its own
> FFmpeg (libdav1d, enabled on `feature/file-backed-video-lerobot` by commit
> `45bb5c4`). The plugin decodes nothing — it only registers the path.

---

## 5. Try it in the app (numeric + video)

1. Launch PlotJuggler 4: `~/Work/PJ4/run.sh`.
2. **File → Open** → filter `*.json`.
3. Navigate to `~/datasets/pusht_v21/meta/` and pick `info.json`. (The plugin
   only claims `.json` so it doesn't overlap with `data_load_parquet` on
   loose parquet files — the dataset is identified by `meta/info.json`, not
   by one of its parquets.)
4. The **LeRobot Dataset** dialog appears:
   - Header: path · `v2.1` · fps · episode count · camera list.
   - Episode list (`ep N · L frames · task`).
   - Select one or several episodes (multi-select). **OK** stays disabled
     when nothing is selected.
5. **Scalar series** (DataEngine):
   - Flattened series appear with names from `info.json`. In pusht v2.1:
     `lerobot/observation.state.motor_0`, `…motor_1`,
     `lerobot/action.motor_0`, `…motor_1`, plus `lerobot/episode_index`,
     `lerobot/frame_index`, `lerobot/next.reward`, etc.
   - Drag `observation.state.*` onto a plot: continuous curve.
6. **Multi-episode** (`__pj_fanout`):
   - Each selected episode is loaded as its own **DatasetId**. They appear
     in the catalog as `pusht_v21/ep_3`, `pusht_v21/ep_5`, … (they are
     **not** concatenated into a single timeline — each has its own clock
     starting at 0).
7. **Camera video** (host streaming video decoder):
   - In the catalog tree, each camera appears as an **object-topic** under
     its episode: e.g. `lerobot/observation.image`.
   - **Drag it onto an empty 2D view** (placeholder in the dock).
   - PJ4 unwraps the lazy `PJ.VideoFrame` entries and feeds them to the
     streaming video decoder (codec-generic: H.264 / H.265 / AV1). Each entry's
     bytes are read from the MP4 on demand, so the clip stays non-resident.
   - Move the time cursor: the video frame follows the cursor in sync with
     the curves. Multi-camera → one 2D view per camera.

> The plugin does **not** decode video. It demux-indexes each MP4 (no decode)
> and pushes one lazy `PJ.VideoFrame` per access unit, schema-tagged
> `kSchemaVideoFrame`; the compressed bytes are read from the file on demand and
> never fully buffered.

---

## 6. Reset / repeat

To re-test from scratch: delete
`~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader/` and
`~/Work/pj-official-plugins/build/data_load_lerobot/`, then repeat from
step 1.

---

## Known issues

- **CPM clones over SSH** if you don't pass `-DCPM_plotjuggler_core_SOURCE=`
  → use the local-path variant (step 1).
- **Video codec (AV1/H.264/HEVC/…)**: decoded by the **host's** FFmpeg (PJ4),
  not by the plugin. AV1 requires the PJ4 build to include libdav1d (the
  `feature/file-backed-video-lerobot` branch carries it). If the camera
  shows up in the tree but nothing renders when you drag it onto the 2D
  view, check that the PJ4 you're running is from that branch and rebuild
  after `conan install` to re-pick libdav1d.
- **Dialog doesn't appear when loading `info.json`**: the `*.json` filter
  and the dialog hook in via the file extension registered in the plugin
  manifest. Check that `~/.local/.../extensions/lerobot-loader/` contains
  the `.pjmanifest.json` sidecar and that it carries
  `"file_extensions": [".json"]`.
