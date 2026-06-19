# anomaly_runner — headless Anomaly Detection pipeline (Task F)

A standalone CLI that runs the **same** Lua anomaly-detection rules you author and
test in the PlotJuggler GUI (the *Anomaly Detector* toolbox), but with **no GUI** —
against a data file on a server / in CI. It loads the file through a PlotJuggler
DataSource plugin, runs the rule via the shared `anomaly_core` engine, and prints a
**structured JSON report** (pass/fail + anomalies + severities). The process **exit
code is `0` on pass and `1` on fail**, so it slots straight into pipelines.

GUI and runner share one engine (`toolbox_anomaly_detector/core/anomaly_core`): a
rule that works in the editor runs unchanged here.

## Build

Built with the rest of the repo (`./build.sh`) or on its own:

```bash
./build.sh tools/anomaly_runner
# binary: build/tools/anomaly_runner/Release/bin/anomaly_runner
```

It links only the SDK plugin host (`plotjuggler_sdk::plugin_host`) + `anomaly_core`
— no `pj_datastore`, no Qt. Data is captured by an in-memory write-host that
implements the DataSource write ABI directly.

## Usage

```
anomaly_runner --data <file> (--script <name|file.lua> | --rule <rule.json>)
               [--source <topic/field>] [--out <report.json>]
               [--fail-on info|warning|error|critical]
               [--csv-time-column <index>] [--plugin <datasource.so>]
anomaly_runner --list-functions
```

- `--data`     data file to analyse — **`.csv` or `.mcap`** (the DataSource plugin is
               chosen by extension; see *Formats* below).
- `--script`   a **built-in** rule name (see `--list-functions`) **or** a path to a
               `.lua` file. `--SOURCE--` in the script is replaced with `--source`.
- `--rule`     a **portable rule file** (the JSON the GUI saves) supplying
               `code`/`source`/`fail_on`; explicit flags still override it. Use this
               instead of `--script` to run the exact rule authored in the GUI.
- `--source`   the series the rule targets, as `topic/field` (e.g. `run/accel_x`).
- `--out`      write the JSON to a file (default: stdout). A one-line `PASS/FAIL`
               summary goes to stderr.
- `--fail-on`  severity at/above which the run is a **fail** (default `error`).
- `--csv-time-column <i>`  use CSV column `i` as the time axis (else row number).
- `--plugin`   the DataSource plugin `.so` (default: `libcsv_source_plugin.so` next
               to the executable).

### Examples

```bash
RUN=build/tools/anomaly_runner/Release/bin/anomaly_runner
CSV=build/all/Release/bin/libcsv_source_plugin.so
DATA=toolbox_anomaly_detector/test_data/anomaly_demo.csv

# Built-in Spike detector → JSON to stdout, exit 1 (anomalies found)
$RUN --data $DATA --plugin $CSV --csv-time-column 0 \
     --script "Spike (point)" --source anomaly_demo/value

# Your own rule, report to a file, custom fail threshold
$RUN --data $DATA --plugin $CSV --csv-time-column 0 \
     --script my_rule.lua --source anomaly_demo/value \
     --out report.json --fail-on warning

# CI gate
$RUN --data run.csv --script "Out of range (region)" --source run/value \
  && echo "clean" || echo "anomalies detected"
```

## Authoring detection scripts

Same primitives as the GUI editor (all run in a sandboxed Lua VM):

```lua
local s = series("topic/field")     -- accessor
s:size()                            -- sample count
s:at(i)        -> { t=<ns>, v=<value> }
s:atTime(t)    -> interpolated value

-- markers (opts is an optional table: {label, color="#rrggbb",
--          severity="info|warning|error|critical", status, category, description})
startMarker(t);  closeMarker(t, opts)        -- a time region
createEvent(x, y, opts)                       -- x only=Vline | y only=Hline | both=point
createDataEvent(low, high, opts)              -- a value band
bandPower(series, fLo, fHi)  -> number        -- FFT power in [fLo,fHi] Hz (vibration)
```

`--list-functions` prints the built-in library (threshold, rate-of-change, spike,
out-of-range, flatline, incoherent, limit lines/band, **spectral band power**,
**boolean flag edges**, …) — the same set as the GUI dropdown.

## JSON report schema

```json
{
  "file": "...", "script": "...", "source": "...",
  "status": "pass" | "fail",
  "summary": {
    "total": <int>,
    "by_severity": { "info": n, "warning": n, "error": n, "critical": n },
    "by_status":   { "none": n, "pass": n, "fail": n }
  },
  "anomalies": [
    { "kind": "event|region|value_band|label", "label": "...", "category": "...",
      "description": "...", "severity": "...", "status": "...",
      "t_start_ns": <int64>, "t_end_ns": <int64>, "has_value": <bool>,
      "value_low": <num>, "value_high": <num>, "color": "#rrggbb"|null }
  ]
}
```

`status` is `fail` when any anomaly meets/exceeds `--fail-on` or has `status=fail`.

### Field semantics — a tagged union keyed by `kind`

Every anomaly object always carries the **same keys** (a fixed schema, so the output loads
straight into a table / DataFrame). But the **anchor** fields are a tagged union: which of them
are meaningful depends on `kind`. Irrelevant fields are left at their schema default
(`0` / `false` / `null`) — they are **not** sentinels to test against, just unused slots.

| `kind` | meaningful anchor fields | ignore |
|---|---|---|
| `region` | `t_start_ns`, `t_end_ns` | `value_low`, `value_high`, `has_value` |
| `event` (point) | `t_start_ns`, `value_low` (the y-value), `has_value=true` | `t_end_ns`, `value_high` |
| `event` (vertical line) | `t_start_ns`, `has_value=false` | `t_end_ns`, `value_low`, `value_high` |
| `value_band` | `value_low`, `value_high` (both real, even when one is `0.0`) | `t_start_ns`, `t_end_ns` |
| `label` | `t_start_ns` | `t_end_ns`, `value_*` |

**Consumer rule: branch on `kind` (and `has_value` for events), then read only that kind's
fields.** A `value_high` of `0.0` is a real upper bound for a `value_band` but unused filler
for an `event` — the discriminator is `kind`, never the number itself.

```python
for a in report["anomalies"]:
    if a["kind"] == "region":
        t0, t1 = a["t_start_ns"], a["t_end_ns"]
    elif a["kind"] == "event":
        t, v = a["t_start_ns"], (a["value_low"] if a["has_value"] else None)
    elif a["kind"] == "value_band":
        lo, hi = a["value_low"], a["value_high"]   # both real, including 0.0
```

The `severity`, `status`, `label`, `category`, `description`, `color` fields apply to every
kind. Object key order in the emitted JSON follows this logical order (insertion order,
preserved via `nlohmann::ordered_json`), not alphabetical.

## Rule files (portable, shareable)

A rule is one JSON document — authored/saved in the GUI (*Anomaly Detector* → Save rule)
and run unchanged here with `--rule`:

```json
{
  "version": 1,
  "name": "spike check",
  "description": "flags sudden jumps",
  "rule": {
    "code": "local s = series(\"--SOURCE--\")\n...createEvent(...)...",
    "source": "anomaly_demo/value",
    "fail_on": "error"
  }
}
```

Version-control these alongside your data pipelines; the GUI and the runner are guaranteed
to interpret them identically (both use the same `anomaly_core` engine).

## Formats

- **CSV** (`libcsv_source_plugin.so`, direct ingest). Topic = file basename; columns become
  fields. Add `--csv-time-column <i>` to use a real time column (needed for spectral, whose
  sampling rate is derived from the timestamps); otherwise the row number is the time axis.
- **MCAP** (`libmcap_source_plugin.so`, *delegated* ingest). The source emits raw message
  bytes; the runner auto-loads the matching MessageParser plugin by encoding
  (`cdr`/`ros2msg`/`ros1msg`→`libparser_ros_plugin.so`, `protobuf`→`libparser_protobuf_plugin.so`,
  `json`→`libparser_json_plugin.so`) from the plugin directory, decodes each message, and
  captures the numeric fields as `topic/field` series. All channels load (no GUI topic
  selection). Example:
  ```bash
  anomaly_runner --data run.mcap --script "Spike (point)" --source /sensor/value/data
  ```
  Channels whose encoding has no available parser are skipped.

  Series names are built with the SDK's `markerSeriesKey(topic, field)` — the same helper the
  GUI uses — so a rule authored against a curve in the GUI targets the **identical** name here.
  It joins with a single `/` and won't double it when a field already starts with `/` (so a ROS
  field `/data` under topic `/sensor/value` is `/sensor/value/data`, not `…//data`). If a
  `--source` isn't found, the runner prints the available series names to help you correct it.

## Notifications / pipeline integration

The JSON-to-stdout-or-file + the `0`/`1` exit code are the "stdout for CI/data
pipelines" integration path. A server-side watcher or cron job runs the binary per
upload and gates on the exit code; webhook/email delivery can wrap that.
