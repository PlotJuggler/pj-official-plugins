# Anomaly Detector

Detect anomalies in your timeseries with small **Lua rules** that emit **plot markers**
(events, regions, value bands). The detection engine (`core/anomaly_core`) is shared by this
GUI toolbox and the headless [`anomaly_runner`](../tools/anomaly_runner/README.md) CLI, so a
rule you author and test here runs **identically** on a server or in CI. Rules are saved as
portable JSON.

## Using it

1. Open **Toolbox → Anomaly Detector** (it reads the loaded timeseries).
2. Pick a **source curve** in the source list — the preview chart fills with it.
3. Pick a **builtin function** (loads its Lua, targeting the selected curve) or write your own
   rule in the editor. The preview overlays the rule's **detected markers** live, so you see
   what Apply will publish before committing.
4. **Apply** — the markers are published onto every plot of that curve. The status line shows
   `Done: N marker(s)` or `Error: …`.
5. **Save rule as… / Load rule…** — native file dialogs for the portable rule JSON (the same
   file the CLI consumes with `--rule`).
6. **Global marker** — when ticked, markers publish to the dataset-global topic (drawn on every
   plot); otherwise only under the selected curve.

Changing the source re-targets a builtin rule to the new curve automatically (the preview
recomputes). If you hand-edit the Lua, your edits are kept — adjust the `series("…")` line
yourself.

## Builtin functions

Same set as the CLI's `--list-functions`. "Kind" is the marker type the template emits; the
constant is the threshold you typically tune.

| Function | Marker kind | Detects | Tunable |
|---|---|---|---|
| `-- No function --` | — | Empty template — write your own rule | — |
| `Showcase (all markers)` | all | One of every marker kind, each colored + labelled (visual check) | — |
| `Severity colors (lines)` | value band | Four horizontal lines in the info/warning/error/critical palette | — |
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

Rules run in a sandboxed Lua VM. Series are read-only; markers are emitted by calling the
creation functions. `--SOURCE--` in a builtin template is replaced with the selected source
(CLI: with `--source`).

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

**Marker options (`opts` table, all optional):**

| Key | Type | Effect |
|---|---|---|
| `label` | string | Pill text on the plot (and the report `label`). No pill if unset. |
| `severity` | `"info"`/`"warning"`/`"error"`/`"critical"` | Default color + report severity (and the CLI `--fail-on` gate). |
| `color` | `"#rrggbb"` | Explicit color override; defaults from `severity`. |
| `status` | `"none"`/`"pass"`/`"fail"` | Verdict; a `fail` fails the CLI run regardless of severity. |
| `category` | string | Free-form class, e.g. `"spectral"`, `"flag"`, `"overspeed"`. |
| `description` | string | Optional longer text carried into the report. |

## Sharing & headless runs

**Save rule as…** writes a portable JSON document (`version`, `name`, `description`, and a
`rule` of `code` / `source` / `fail_on`). Version-control it and run it unchanged on a server
or in CI — same engine, same markers:

```bash
anomaly_runner --rule rule.json --data run.mcap
```

The runner reads CSV/MCAP, emits a structured JSON report, and exits `0` pass / `1` fail / `2`
usage-error for pipeline gating. See [`tools/anomaly_runner`](../tools/anomaly_runner/README.md).

## Build

```bash
cd ~/Work/pj-official-plugins
./build.sh toolbox_anomaly_detector   # the GUI plugin
./build.sh tools/anomaly_runner       # the headless CLI (same anomaly_core)
```
