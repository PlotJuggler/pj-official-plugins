# Changelog — toolbox_anomaly_detector

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_anomaly_detector/`.

## [Unreleased]

### Fixed
- A Global rule no longer replaces a Dataset rule (they shared generator id
  `rule/__global__`).
- A Global rule is drawn on every loaded dataset, including those that lack the
  series it reads (host-side; earlier hosts errored on such a dataset).

## [0.2.0] - 2026-09-10

### Fixed
- The Source list offered every scalar field in the catalog but only cached
  samples for float64, so it silently split series into three groups the user
  could not tell apart: float64 worked, float32/int/bool ran on Apply but
  previewed blank, and a text series previewed blank and then died on Apply
  with "attempt to index nil value". The dialog also auto-selected the first
  name in the list, so a recording whose first field is text opened already
  broken. The list is now filtered by the host's own criterion (numeric and
  bool, no text), which is the same rule the marker engine and the host's
  resolver apply.

### Changed
- One scope picker instead of the previous split control, section header bands
  to group the dialog, and the panel now uses the whole window. "Curve" is
  called "series" throughout, and the redundant Preview label is gone.

## [0.1.0] - 2026-08-05
