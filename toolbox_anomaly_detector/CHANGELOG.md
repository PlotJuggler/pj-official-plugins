# Changelog — toolbox_anomaly_detector

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_anomaly_detector/`.

## [0.3.2] - 2026-11-23

### Fixed
- Severity colors, Limit lines and Limit band now work in the Timeseries and
  Dataset scopes instead of failing with "none of the generator's inputs matches
  a loaded series".

## [0.3.1] - 2026-11-18

### Fixed
- 0.3.0 could be offered to PlotJuggler 4.1, whose rules lack `when()`, so its
  built-in functions failed there. The marketplace picks a release by SDK, so
  0.3.1 requires SDK 1.0.0 (PlotJuggler 4.2): 4.1 keeps 0.2.1.

## [0.3.0] - 2026-11-18

### Changed
- Renamed **Marker Editor** (was Anomaly Detector): its rules draw any plot
  marker, anomalies being one use among others (phases, coloring the background
  by a series' value). The plugin id `toolbox-anomaly-detector` and the
  `anomaly_runner` CLI keep their names, so saved layouts, installs and CI
  scripts are unaffected.
- Requires PlotJuggler 4.2.0 or newer (earlier hosts keep 0.2.1).

### Added
- Rules can say "when this series does X, draw Y" in one call, without a loop:
  `when(series, above(x)):backgroundColor("red")`, `:event()`, `:verticalLine()`, with
  conditions `above`, `below`, `between`, `outside`, `equals`, `isTrue`, `changes` or a
  function. The Threshold, Out of range, Spike, Flatline, Rate of change and Boolean flag
  functions use it; Threshold now draws one line per crossing, not one per sample.
- A **Color map (region)** function, PlotJuggler 3's ColorMap: `when(series)` with no
  condition takes a value-to-color table or function.
- `when("name", ...)` declares `name` as a rule input, like `series("name")`.

## [0.2.1] - 2026-09-17

### Fixed
- The footer buttons (Save rule as..., Load rule..., Apply, Close) were drawn
  touching each other since 0.2.0. They now keep the same gap as every other
  action row in the app.

## [0.2.0] - 2026-09-15

### Fixed
- A Global rule no longer replaces a Dataset rule (they shared generator id
  `rule/__global__`).
- A Global rule is drawn on every loaded dataset, including those that lack the
  series it reads (host-side; earlier hosts errored on such a dataset).
- The Source list offered every scalar field in the catalog but only cached
  samples for float64, so it silently split series into three groups the user
  could not tell apart: float64 worked, float32/int/bool ran on Apply but
  previewed blank, and a text series previewed blank and then died on Apply
  with "attempt to index nil value". The dialog also auto-selected the first
  name in the list, so a recording whose first field is text opened already
  broken. The list is now filtered by the host's own criterion (numeric and
  bool, no text), which is the same rule the marker engine and the host's
  resolver apply.
- Picking a source before any function left the rule on `series("--SOURCE--")`
  with a "none of the generator's inputs matches" error until a function was
  picked; the source now retargets the pristine template as it should.
- A `series("...")` inside a Lua comment counted as a rule input, so the blank
  template's help comment made the preview fail with "input 'topic/field' does
  not exist", and a commented-out line could fail the rule the same way.
  Comments are now skipped when the inputs are collected.

### Changed
- One scope picker instead of the previous split control, section header bands
  to group the dialog, and the panel now uses the whole window. "Curve" is
  called "series" throughout, and the redundant Preview label is gone.

## [0.1.0] - 2026-08-05
