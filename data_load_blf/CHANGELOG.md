# Changelog — data_load_blf

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_blf/`.

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
