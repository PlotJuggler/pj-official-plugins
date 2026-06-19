# Anomaly Detector

Author **anomaly-detection rules in Lua** over your timeseries and visualize the results as
**plot markers** (regions, events, value bands) directly on PlotJuggler plots. The same rule
runs unchanged, with no GUI, on a server or in CI via the headless
[`anomaly_runner`](../tools/anomaly_runner/README.md) CLI.

GUI and runner share one engine (`core/anomaly_core`): a rule that fires in the editor
produces the **identical** markers headless. Rules are saved as portable JSON, so you author
and test interactively, then gate every uploaded log on the exact same rule in your pipeline.

## What it is

Two surfaces over one engine:

- **GUI toolbox** (this plugin) — a Filter-Editor-style dialog: preview chart, source-curve
  list, builtin-function list, and a Lua editor. Pick a builtin or write your own, **Apply**,
  and the emitted markers are published onto the plots.
- **`anomaly_core`** (`core/anomaly_core.{hpp,cpp}`) — a GUI-free static lib that runs the Lua
  rule against a series provider and returns `PlotMarker`s, plus the JSON report and the
  portable-rule (de)serialization. Linked by both the plugin and the CLI.

## Using the GUI

1. Open **Toolbox → Anomaly Detector** (it reads the loaded timeseries).
2. **Select a source curve first** in the source list. The preview chart fills with it.
3. Pick a **builtin function** (fills the editor with its Lua, targeting the selected source),
   or write your own rule in the editor.
4. Click **Apply** — the rule runs and its markers appear on every plot of that curve.
   The status line shows `Done: N marker(s)` or `Error: …`.
5. **Save rule** / **Load rule** export and restore the rule as a portable JSON file — the
   same file the CLI consumes with `--rule`.
6. **Global marker** checkbox: when ticked, markers publish to the dataset-global topic (drawn
   on every plot); otherwise they publish under the selected curve (drawn only on its plots).

> **Order matters: select the source before picking a function.** A builtin template targets
> the source that is selected *at the moment you pick the function* (its `--SOURCE--`
> placeholder is substituted then). Changing the source afterward does **not** rewrite the
> editor — re-pick the function, or edit the `series("…")` line, so the rule targets the curve
> you intend.

## Builtin functions

The function list (same set as the CLI's `--list-functions`). "Kind" is the marker type each
template emits; the listed constant is the threshold you typically tune.

| Function | Marker kind | Detects | Tunable |
|---|---|---|---|
| `-- No function --` | — | Empty template — write your own rule | — |
| `Showcase (all markers)` | all | One of every marker kind, each colored + labelled (visual check) | — |
| `Severity colors (lines)` | value band | Four horizontal lines using the builtin info/warning/error/critical palette | — |
| `Threshold (line)` | event (vline) | A vertical line at every sample above a threshold | `TH = 0.5` |
| `Out of range (region)` | region | Shades the span while the value is outside `[LO, HI]` | `LO,HI = -0.5,0.5` |
| `Spike (point)` | event (point) | A point at a sudden jump between consecutive samples | `JUMP = 0.8` |
| `Incoherent point` | event (point) | An isolated outlier far from **both** neighbours | `DEV = 1.0` |
| `Flatline (region)` | region | Shades a span where the value is stuck (barely changes) | `EPS = 1e-4` |
| `Rate of change (line)` | event (vline) | A line where the slope `dv/dt` exceeds a limit | `RATE = 5.0` |
| `Limit lines (horizontal)` | value band | Two horizontal lines at the allowed limits `LO`, `HI` | `LO,HI = -0.5,0.5` |
| `Limit band` | value band | The allowed band `[LO, HI]` as a shaded horizontal band | `LO,HI = -0.5,0.5` |
| `Spectral band power (vibration)` | region | Flags excessive FFT energy in a frequency band `[F_LO, F_HI]` Hz | `LIMIT = 0.02` |
| `Boolean flag (edges)` | event + region | Marks each rising edge of a flag and shades while it stays raised | — |

## Lua API

Rules run in a sandboxed Lua VM (sol2). Series are read-only; markers are emitted by calling
the creation functions. `--SOURCE--` in a builtin template is replaced with the selected
source name (CLI: with `--source`).

```lua
-- Series accessor
local s = series("topic/field")   -- nil if the name doesn't exist
s:size()                          -- sample count (int)
s:at(i)                           -- 0-based: { t = <ns>, v = <value> }, or nil if out of range
s:atTime(t)                       -- value linearly interpolated at time t (ns)

-- Marker creation
startMarker(t)                    -- open a time region at t (ns)
closeMarker(t, opts?)             -- close the open region at t, apply opts
createEvent(x?, y?, opts?)        -- x only = vertical line | y only = horizontal line | both = point
createDataEvent(low, high, opts?) -- a shaded value band [low, high]

-- Spectral helper
bandPower(s, fLo, fHi)            -- summed FFT power in [fLo,fHi] Hz, DC-removed (needs a real time axis)

-- Introspection
GetSeriesNames()                  -- list of all available series names
```

### Marker options (`opts` table)

Every field is optional:

| Key | Type | Effect |
|---|---|---|
| `label` | string | Pill text drawn on the plot (and the report `label`). No pill if unset. |
| `severity` | `"info"`/`"warning"`/`"error"`/`"critical"` | Drives the default color and the report severity (and the CLI `--fail-on` gate). |
| `color` | `"#rrggbb"` | Explicit color override; if unset, color derives from `severity`. |
| `status` | `"none"`/`"pass"`/`"fail"` | Verdict; a `fail` makes the CLI run fail regardless of severity. |
| `category` | string | Free-form class, e.g. `"spectral"`, `"flag"`, `"overspeed"`. |
| `description` | string | Optional longer text carried into the report. |

## Marker visuals

The PJ4 renderer (`pj_plotting/widget/.../PlotMarkersItem.cpp`) draws each kind as: colored
**pill** label badges (white text; vertical-line labels rotated 90°), point events as a
**hollow ring** (transparent centre) on the sample, value bands / horizontal lines as shaded
bands, and time regions as shaded spans. Markers whose anchor falls outside the visible canvas
are **culled** (so pills don't leak onto the plot border when zoomed).

## Build & deploy

```bash
cd ~/Work/pj-official-plugins
./build.sh toolbox_anomaly_detector
cp build/toolbox_anomaly_detector/Release/bin/libtoolbox_anomaly_detector_plugin.so build/all/Release/bin/
```

The headless CLI builds from the same `anomaly_core`:

```bash
./build.sh tools/anomaly_runner
cp build/tools/anomaly_runner/Release/bin/anomaly_runner build/all/Release/bin/
```

## Feature coverage

- **GUI** — source/function/editor dialog, live preview, Apply, Save/Load portable rule,
  Global-marker toggle, error/status line (`anomaly_detector.cpp`).
- **Core** — sol2 Lua engine, series + marker primitives, `bandPower` (kissfft), 13 builtins,
  JSON report, portable-rule (de)serialization (`core/anomaly_core.{hpp,cpp}`).
- **CLI** — headless `anomaly_runner`: CSV + MCAP ingest, exit-code gate, JSON report
  (`../tools/anomaly_runner/`).
- **Rendering** — pills, hollow points, bands, regions, off-screen culling
  (PJ4 `pj_plotting`).
- **SDK** — `PlotMarkers` builtin object + codec, marker object-write surface (`plotjuggler_sdk`).
