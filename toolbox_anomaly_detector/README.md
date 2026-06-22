# Anomaly Detector

Detect anomalies in your timeseries with small **Lua rules** that emit **plot markers**
(events, regions, value bands). Author and test a rule interactively in the **PlotJuggler
GUI**, then run that exact same rule **headless** on a server / in CI. Rules are saved as a
portable JSON file that both sides consume.

```
            author + test                          run unattended
   ┌──────────────────────────┐   rule.json   ┌──────────────────────────┐
   │  GUI: Anomaly Detector   │ ────────────▶ │  CLI: anomaly_runner     │
   │  (host runs the rule)    │               │  (runs the rule itself)  │
   └──────────────────────────┘               └──────────────────────────┘
              same Lua rule  ·  same PlotMarkers  ·  same result
```

### How it runs (one paragraph)

The **GUI toolbox is host-driven and carries no script engine**: it submits the rule to
PlotJuggler via the `pj.markers.v1` SDK service, and the host runs it, publishes the markers,
and re-runs them as data changes (the live preview is host-driven too). The **headless
`anomaly_runner` is standalone**: it links the engine and runs the rule itself, so it needs no
GUI. Both run the rule through the **same `runMarkerScript` core**, so a rule authored in the
GUI produces identical markers in CI — only *who invokes the engine* differs.

---

## Using the GUI

1. Open **Toolbox → Anomaly Detector** (it reads the currently loaded timeseries).
2. Pick a **source curve** in the source list — the preview chart fills with it.
3. Pick a **builtin function** (loads its Lua, targeting the selected curve) or write your own
   rule in the editor. The preview overlays the rule's **detected markers live** (computed by
   the host). A rule error shows in the status line as `Rule error: …` so a blank overlay is
   never mistaken for "no anomalies".
4. Choose where the markers land:
   - **default (per-series):** under the selected source curve only.
   - **Global marker:** on the dataset-global topic — drawn on *every* plot of the dataset.
   - **Global marker + All datasets:** global on *every loaded dataset* (handy when several
     logs are open at once).
5. **Apply** — submits the rule to the host as a live generator. It recomputes automatically
   when the data changes/reloads. The status shows `Done: …` or `Error: …`.
6. **Save rule as… / Load rule…** — native file dialogs for the portable rule JSON (the same
   file the CLI consumes with `--rule`).

Changing the source re-targets a builtin rule to the new curve automatically (the preview
recomputes). If you hand-edit the Lua, your edits are kept — adjust the `series("…")` line
yourself.

---

## Using the headless runner

The `anomaly_runner` CLI runs the same rules on a file (CSV/MCAP) with no GUI, prints a
**structured JSON report**, and sets the **process exit code** for pipeline gating:
`0` = pass, `1` = fail (anomalies at/above the fail threshold), `2` = usage error.

Standing in the build output dir (`build/all/Release/bin/`):

```bash
# List the builtin functions (names match the GUI dropdown)
./anomaly_runner --list-functions

# Run a builtin rule on a series (prints the JSON report to stdout)
./anomaly_runner --data run.mcap --script "Spike (point)" --source "imu/accel/x"

# Run a portable rule .json (what the GUI's "Save rule as…" produces) — the CI flow
./anomaly_runner --data run.mcap --rule rule.json --out report.json
echo $?        # 0 = clean, 1 = anomalies → block the upload

# Tune the fail threshold (only fail at critical), and save the report
./anomaly_runner --data run.mcap --rule rule.json --fail-on critical --out report.json

# Load a non-default DataSource plugin explicitly (e.g. for MCAP)
./anomaly_runner --plugin ./libmcap_source_plugin.so --data run.mcap --rule rule.json
```

> ⚠️ **Exit-code gotcha:** `./anomaly_runner … | grep …` makes `echo $?` report **grep's**
> exit, not the runner's. To gate on the runner, redirect (`> report.json`) and then
> `echo $?`, or use `echo ${PIPESTATUS[0]}`.

**Notifications & batch screening (Task F):** on a server the runner can deliver the report to
a webhook / email / command on a bad log via `--notify config.json`, and
`tools/anomaly_runner/deploy/watch.sh` screens every uploaded MCAP automatically. Full CLI
reference (report JSON schema, notify config, watcher): **[`tools/anomaly_runner/README.md`](../tools/anomaly_runner/README.md)**.

---

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
(GUI: the source list; CLI: `--source`).

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

## Portable rule file

**Save rule as…** writes a self-contained JSON document — `version`, `name`, `description`, and
a `rule` of `code` / `source` / `fail_on`. The GUI saves it and the runner consumes it with
`--rule`, so a rule authored interactively runs unchanged on a server. Version-control it like
any other config.

## Build

```bash
cd ~/Work/pj-official-plugins
./build.sh toolbox_anomaly_detector   # the GUI plugin (Luau-free; host-driven)
./build.sh tools/anomaly_runner       # the headless CLI (standalone engine)
```

The two-layer core (`core/anomaly_helpers`, Luau-free, linked by the plugin; `core/anomaly_core`,
the engine, linked by the runner) is what lets the GUI plugin ship without a script engine.
