# Vendored SDK floor-checker core

`feature_floor_check.py` is a byte-identical copy of `plotjuggler_sdk`
`tools/feature_floors/feature_floor_check.py` (shipped in the package at
`share/plotjuggler_sdk/` from 0.33.0).

It stays vendored because `scripts/release_tools.py` needs this core on
runners that have no SDK available (the release build legs and the
`submit-to-registry` job) — `check_sdk_feature_floors.py` is not the only
consumer.

Refresh it on every SDK bump. `check_sdk_feature_floors.py` byte-compares it
against the SDK's own copy every time it resolves a table, so a stale copy
fails the check.
