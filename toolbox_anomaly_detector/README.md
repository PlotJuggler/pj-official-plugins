# Marker Editor

Draw **plot markers** (events, regions, value bands) from small **Lua rules** over your
timeseries: flag anomalies, mark phases, or shade the background by a series' value (what
PlotJuggler 3 called a color map). Author and test a rule interactively in the **PlotJuggler
GUI**, then run that exact same rule **headless** on a server / in CI. Rules are saved as a
portable JSON file that both sides consume.

```
            author + test                          run unattended
   ┌──────────────────────────┐   rule.json   ┌──────────────────────────┐
   │  GUI: Marker Editor      │ ────────────▶ │  CLI: anomaly_runner     │
   │  (host runs the rule)    │               │  (runs the rule itself)  │
   └──────────────────────────┘               └──────────────────────────┘
              same Lua rule  ·  same PlotMarkers  ·  same result
```

### How it runs (one paragraph)

The **GUI toolbox is host-driven and carries no script engine**: it submits the rule to
PlotJuggler via the unified `pj.data_processors.v1` SDK service (`kind="markers"`), and the host runs it, publishes the markers,
and re-runs them as data changes (the live preview is host-driven too). The **headless
`anomaly_runner` is standalone**: it links the engine and runs the rule itself, so it needs no
GUI. Both run the rule through the **same `runMarkerScript` core**, so a rule authored in the
GUI produces identical markers in CI — only *who invokes the engine* differs.

---

## Using the GUI

1. Open **Toolbox → Marker Editor** (it reads the currently loaded timeseries). It opens
   with **no source selected**.
2. Choose a **source series**, either way round: **drag & drop** one from the Datasets tree on
   the left, or pick it from the source list (with the **Filter** box to narrow a long
   catalog). The preview chart fills with it.

   Only series a rule can actually read are listed: numeric and boolean ones. **Text series
   are not offered** — the detection engine works in doubles, and the host rejects a
   non-plottable field as a rule input, so a text source could only ever fail. Markers are
   drawn on the time-series plot overlay, and text series are not shown there either (they
   render in the State Transitions strip).
3. Pick a **builtin function** (loads its Lua, targeting the selected series) or write your own
   rule in the editor. The preview overlays the rule's **detected markers live** (computed by
   the host). A rule error shows in the status line as `Rule error: …` so a blank overlay is
   never mistaken for "no anomalies".
4. Choose the **scope** — one segmented control on the Lua rule band, widening left to right:
   - **Timeseries:** under the selected source series only.
   - **Dataset:** on the dataset marker topic — drawn on *every* plot of the dataset, shown
     as the *Dataset markers* row in the plot's Curves panel.
   - **Global:** on the host's all-datasets marker topic. The rule runs on every loaded
     dataset that has the series it reads, and the combined result is drawn on *every loaded
     dataset*, including those without that series and those loaded later — shown as the
     *Global markers* row, and can coexist with a Dataset rule. Global markers keep their
     display instant: on a dataset whose clock is not yet aligned in the Source Timeline they
     can fall outside its data range.

   The two rows and the all-datasets drawing need a PlotJuggler newer than 3.999.9. On an
   older host both scopes share the dataset marker topic, so the last rule applied wins.
5. **Apply** — submits the rule to the host as a live generator. It recomputes automatically
   when the data changes/reloads. The status shows `Done: …` or `Error: …`.
6. **Save rule as… / Load rule…** — native file dialogs for the portable rule JSON (the same
   file the CLI consumes with `--rule`).

Changing the source re-targets a builtin rule to the new series automatically (the preview
recomputes). If you hand-edit the Lua — or load a rule from a file — your rule is kept and
picking a source will not overwrite it; adjust the `series("…")` line yourself.

**Apply** stays disabled until there is something to run against: a resolved source series, or
a scope of **Dataset** or **Global**. A source restored from a saved layout that the loaded datasets do
not currently offer is kept, not silently dropped — the status line says so, and it starts
working again as soon as that dataset is loaded.

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
| `Color map (region)` | region | Shades the background by value: one color per range (`when` + `backgroundColor`) | the ranges |
| `Threshold (line)` | event (vline) | A vertical line where the value rises above a threshold | `TH = 0.5` |
| `Out of range (region)` | region | Shades the span while the value is outside `[LO, HI]` | `LO,HI = -0.5,0.5` |
| `Spike (point)` | event (point) | A point at a sudden jump between consecutive samples | `JUMP = 0.8` |
| `Incoherent point` | event (point) | An isolated outlier far from **both** neighbours | `DEV = 1.0` |
| `Flatline (region)` | region | Shades a span where the value is stuck (barely changes) | `EPS = 1e-4`, `MIN_S = 0.5` |
| `Rate of change (line)` | event (vline) | A line where the slope `dv/dt` starts to exceed a limit | `RATE = 5.0` |
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
createMarker(x?, y?, opts?)       -- x only = vertical line | y only = horizontal line | both = point
createVerticalMarker(x, opts?)    -- a vertical line at x      (shortcut for createMarker(x))
createHorizontalMarker(y, opts?)  -- a horizontal line at y    (shortcut for createMarker(nil, y))
createPointMarker(x, y, opts?)    -- a point at (x, y)         (shortcut for createMarker(x, y))
createBandMarker(low, high, opts?) -- a shaded value band [low, high]

-- Conditions and actions: one call instead of a loop
when(s_or_name, condition?)       -- condition: above(x) below(x) between(a, b) outside(a, b) equals(x)
                                  -- isTrue() changes(), or function(v, t, prev, prev_t) -> boolean
  :backgroundColor(color?, opts?) -- shade while the condition holds (no color: the severity color);
                                  -- opts.minDuration (s) drops shorter regions
  :event(opts?)                   -- a point where the condition starts to hold
  :verticalLine(opts?)            -- a line where the condition starts to hold
                                  -- (opts.every = true: at every sample; opts.minGap (s) skips marks
                                  -- closer than that; opts.label may be a function(v, prev))
when(s_or_name):backgroundColor(map) -- no condition: color by value, map = { [value] = color } or a
                                  -- function(v, t) returning a color, { color = .., label = .. }, or nil

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
| `color` | `"#rrggbb"`, `"#rgb"` or a Qt color name (`"orange"`, `"steelblue"`, ...) | Explicit color override; defaults from `severity`. An unknown color fails the rule. |
| `status` | `"none"`/`"pass"`/`"fail"` | Verdict; a `fail` fails the CLI run regardless of severity. |
| `category` | string | Free-form class, e.g. `"spectral"`, `"flag"`, `"overspeed"`. |
| `description` | string | Optional longer text carried into the report. |

## Writing rules with `when`

Most rules are "when this series does X, draw Y": write them as one `when(...)` call instead of a
loop. The engine does the scan, opens and closes regions, and fires a mark once per stretch
rather than on every matching sample:

```lua
when("/robot/speed", above(2.0)):backgroundColor("red", { label = "too fast" })
when("/robot/estop", isTrue()):backgroundColor("red", { label = "E-STOP", minDuration = 0.5 })
when("/imu/acc_x", outside(-3, 3)):event({ label = "overload", minGap = 1.0 })
when("/robot/mode", changes()):verticalLine({ label = function(v, prev) return prev .. " -> " .. v end })
```

Any function works as the condition. It gets the value, the time (ns) and the previous sample,
and may read other series:

```lua
local speed = series("/robot/speed")

local function movingInFault(v, t)
  return v == 2 and speed:atTime(t) > 1.5
end

when("/robot/mode", movingInFault):backgroundColor("red", { label = "moving while in fault" })
```

Keep a loop for windows and multi-step state (a moving average, "A then B within 2 s"), with
`startMarker` / `closeMarker` and the `create*Marker` functions.

## Migrating from PlotJuggler 3's ColorMap

A 3.x ColorMap was a function from a value to a color. Give the same function, or a table, to
`backgroundColor` with no condition:

```lua
-- a discrete state, one color per value
when("/robot/mode"):backgroundColor({ [0] = "gray", [1] = "green", [2] = "orange", [3] = "red" })

-- a level, by range
when("/robot/speed", below(1.0)):backgroundColor("green")
when("/robot/speed", between(1.0, 2.0)):backgroundColor("yellow")
when("/robot/speed", above(2.0)):backgroundColor("red")

-- anything else: your own function, the old ColorMap body
local function modeColor(v)
  if v == 1 then
    return "green"
  elseif v >= 2 then
    return { color = "red", label = "fault" }
  end
end

when("/robot/mode"):backgroundColor(modeColor)
```

Pick **Global** or **Dataset** scope to shade every plot, as the 3.x color map did. `when` and
color names need PlotJuggler 4.2 or newer.

## Portable rule file

**Save rule as…** writes a self-contained JSON document — `version`, `name`, `description`, and
a `rule` of `code` / `source` / `fail_on`. The GUI saves it and the runner consumes it with
`--rule`, so a rule authored interactively runs unchanged on a server. Version-control it like
any other config.

## Build

```bash
# from the repository root
./build.sh toolbox_anomaly_detector   # the GUI plugin (Luau-free; host-driven)
./build.sh tools/anomaly_runner       # the headless CLI (standalone engine)
```

The two-layer core (`core/anomaly_helpers`, Luau-free, linked by the plugin; `core/anomaly_core`,
the engine, linked by the runner) is what lets the GUI plugin ship without a script engine.
