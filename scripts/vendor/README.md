# Vendored SDK floor-checker core

`feature_floor_check.py` is a TEMPORARY byte-identical copy of
`plotjuggler_sdk` `tools/feature_floors/feature_floor_check.py` (shipped in
the package at `share/plotjuggler_sdk/` from 0.33.0). Delete this directory
and load the module from the SDK once `SDK_VERSION` >= 0.33.0 —
`check_sdk_feature_floors.py` hard-fails at that point until the rewiring
happens, and byte-compares this copy against the SDK's whenever a table path
is supplied.
