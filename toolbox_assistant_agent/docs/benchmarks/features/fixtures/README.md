# Feature-catalog fixtures

Host documents in schema 1 of the `pj.feature_catalog.v1` service, used by `../score.py` and the
benchmark runner.

| File | Registry | Plugin lists |
|------|----------|--------------|
| `host_bundled.json` | `fetched` | `loaded` = the Linux AppImage bundle (`BUNDLE_IDS` in PJ4 `packaging/appimage/build_appimage.sh`); `available` = every other plugin in this repo that installs on `linux-x86_64` |
| `host_offline.json` | `unreachable` / `network` | same `loaded`, `in_registry: false`, `available: []` |
| `host_disabled.json` | `fetched` | as bundled, with `mqtt-subscriber` in `installed_not_loaded` (`reason: disabled`) |

Plugin `name`, `version`, `category` and `summary` come from this repo's `manifest.json` files, which
stand in for the registry. `entry_point` is the manifest `entry_point` override when present, otherwise
the host default from `pj_runtime/src/FeatureCatalogHost.cpp` (`defaultEntryPoint`).

## Core entries and limitations

`core` and `limitations` are copied from the PJ4 host resource
`resources/feature_catalog.json`, which is the source of truth. They carry the fields the host forwards
to plugins (core: `id`, `name`, `summary`, `entry_point`; limitation: `id`, `summary`), sorted by id. The
resource's `evidence` and `verified` fields are for maintainers and are not forwarded, so they are not in
the fixtures. Refresh the three fixtures from the resource when it changes; do not edit the entries here.

The resource marks `viewer_2d`, `scene_3d` and `state_transitions` as `verified` in-app (the button and
its tooltip; the drag onto it is not automated). Every other entry has an empty `verified`.

Plugin data loaders use the host string `Sources panel → File (or File → Open Data File...)`
(`kFileImportEntry`), with three ASCII dots.
