# Changelog — data_load_mf4

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_mf4/`.

## [0.1.4] - 2026-10-19

### Fixed

- Dictionaries beside the data are saved relative to it, so a moved or copied layout folder keeps decoding with the DBC next to its data (absolute paths anywhere under the folder that moved, such as a `dbc/` folder next to `data/`, are rebased on replay).
- A missing or unreadable DBC is reported as "DBC file not found" / "cannot read DBC file" instead of a parse error.

## [0.1.3] - 2026-09-21

### Added

- A signal decoded from a DBC `VAL_` value table (on a CAN bus-logging
  channel group) now also gets a `<signal>_label` text field (decoded label,
  or the raw value as text when unmatched); negative and all-unsigned `VAL_`
  key conventions are both accepted. See the README for the `_label` naming
  convention.

### Fixed
- A DBC whose value table (`VAL_`) has more than about 50 entries no longer
  fails to load entirely on Windows.

## [0.1.2] - 2026-09-21

### Fixed

- The shared `common/can_dbc` decoder's vendored DBC parser silently dropped
  any signal whose factor/offset/min/max did not match its number patterns —
  a negative factor, an exponential value (e.g. `[-3.4E+38|3.4E+38]`, common
  in Vector-exported DBCs), a leading `+` sign, or a leading-dot value (`.5`).
  It also required exactly one whitespace character between `BO_`/`SG_`
  tokens, rejecting tabs or repeated spaces. Both are fixed in the vendored
  parser; DBCs affected by either now decode instead of quietly losing
  signals.

## [0.1.1] - 2026-09-06
