# SDK 0.21 and Marketplace Admission TODOs

This is a documentation-only implementation checklist for `pj-official-plugins`.
It records the plugin work expected after the PlotJuggler SDK 0.21 migration and
the broader managed-marketplace design are adopted. It does not change plugin
behavior, dependencies, packaging, or release workflows.

The priorities below are intentional:

- **P0 — required:** complete before claiming SDK 0.21 adoption or production-
  ready marketplace admission, as applicable.
- **P1 — hardening:** complete unless testing demonstrates that the risk is
  already eliminated by a stronger invariant.
- **P2 — later cleanup:** safe to defer until the SDK 1.0 compatibility break.

## P0 — Adopt SDK 0.21 safely

### Release and pinning

- [ ] Do not update this repository until SDK `v0.21.0` has actually been tagged
  and published.
- [ ] Update both `SDK_VERSION` and the `extern/plotjuggler_core` submodule to
  the exact `v0.21.0` tag, using `scripts/bump_core_version.py` so the two pins
  cannot diverge.
- [ ] Make CI reject a mismatch between `SDK_VERSION` and the SDK submodule tag.
- [ ] Rebuild and publish every parser plugin against SDK 0.21. Use each
  plugin's next available patch version; if no intervening release changes the
  current versions, the expected bumps are:
  - `parser_data_tamer`: `0.9.1` to `0.9.2`
  - `parser_json`: `1.1.0` to `1.1.1`
  - `parser_protobuf`: `1.1.0` to `1.1.1`
  - `parser_ros`: `1.1.0` to `1.1.1`
- [ ] Never republish different bytes under an existing plugin version. A
  packaging-only rebuild still requires a patch-version bump.

### Write the migration tests first

- [ ] Replace all parser tests that cast `handle.context()` to
  `PJ::MessageParserPluginBase*` and invoke C++ methods across the DSO boundary.
  The known baseline is 46 casts: 17 in
  `parser_protobuf/tests/protobuf_parser_test.cpp` and 29 in
  `parser_ros/tests/ros_parser_test.cpp`.
- [ ] Exercise object parsing through `handle.supportsFunctionalParsing()` and
  `handle.parseObjectFunctional(...)` instead.
- [ ] Add functional scalar-parsing tests for DataTamer, JSON, Protobuf, and ROS.
  For the same payload and schema, compare the functional result with the
  legacy host-push result while the deprecated bridge remains available.
- [ ] Add negative tests for missing schemas, malformed payloads, wrong schema
  families, unsupported functional operations, and plugin destruction after a
  parse failure.
- [ ] Update object-result lifetime tests. Do not require a returned object span
  to alias the input buffer: the C extension may canonicalize the plugin result
  and the host may decode it into host-owned storage. Assert value equality and
  prove that the result remains valid after the input buffer and temporary
  plugin storage are released.
- [ ] Add a regression test that unloads/destroys each test plugin between test
  cases where supported. The test suite must not depend on symbols leaked by a
  previously loaded DSO.

### Migrate parser implementations

- [ ] Keep the existing functional handler registrations in DataTamer. No
  runtime rewrite is expected unless the new parity tests expose a difference.
- [ ] Keep the existing generic functional handler in JSON. No runtime rewrite
  is expected unless the new parity tests expose a difference.
- [ ] Keep the existing per-schema scalar/object functional handlers in ROS.
  No runtime rewrite is expected unless the new parity tests expose a
  difference.
- [ ] Migrate Protobuf's arbitrary/generic-schema path from imperative
  `parse()`-only behavior to a registered functional scalar handler. A suitable
  internal shape is `decodeGenericScalars(payload) -> ScalarRecord`.
- [ ] Retain the deprecated `parse()` path as a compatibility adapter during
  the 0.21 migration; do not maintain two independent decoding
  implementations.
- [ ] Add correctness and performance benchmarks for generic Protobuf before
  replacing its current bound-field-handle path. Treat a material regression
  as a design issue, not as an acceptable cost of the API migration.

### Remove obsolete DSO workarounds

- [ ] Once the tests no longer pass C++ parser objects or `std::any` across the
  DSO boundary, remove the macOS RTTI/default-visibility workarounds from
  `parser_protobuf/CMakeLists.txt` and `parser_ros/CMakeLists.txt`.
- [ ] Add or extend symbol-export tests so each plugin exports only the C ABI
  entrypoints and intentionally public platform symbols.
- [ ] Verify the parser suite on Linux, macOS, and Windows before publishing the
  rebuilt artifacts.

### Declare the real minimum SDK

- [ ] Add `min_sdk_required` to all maintained plugin manifests.
- [ ] Set it to the oldest SDK contract that the artifact genuinely requires,
  not automatically to the SDK version used to compile it.
- [ ] For existing plugins that remain correctly usable through the SDK 0.20
  contract, declare `"min_sdk_required": "0.20.0"` even when the release is
  compiled with SDK 0.21.
- [ ] Use `0.21.0` only when a plugin genuinely depends on a 0.21-only contract
  and cannot degrade correctly on 0.20.
- [ ] Keep `min_sdk_required` distinct from `min_plotjuggler_version`; they gate
  different compatibility axes.

## P0 — Make release artifacts admissible by the managed marketplace

These TODOs depend on the canonical `pj-plugin-check` helper being supplied by
the PJ4/SDK work. This repository should consume that helper rather than grow a
second DSO loader or metadata implementation.

### Validate the final ZIP, not an intermediate tree

- [ ] Change `.github/workflows/build-release.yml` so every platform matrix leg
  runs `pj-plugin-check` against the final ZIP before checksum generation and
  upload.
- [ ] Replace or delegate the `ctypes.CDLL` inspection in
  `scripts/release_tools.py` to the same helper. Do not load candidate DSOs into
  the Python release process.
- [ ] Require the helper to run in a fresh process for each admission attempt so
  constructor crashes, leaked symbols, and retained loader state cannot poison
  the next check.
- [ ] Fail the release if the helper cannot complete create, bind, and destroy
  for the public plugin module.
- [ ] Fail the release if the embedded manifest and packaged `manifest.json`
  disagree on identity, version, family, ABI, or SDK floor.
- [ ] Fail the release if any declared file digest, package digest, or expected
  payload is missing or different in the final ZIP.
- [ ] Preserve the existing ZIP shape:
  `<extension-id>/` containing the public DSO, `manifest.json`, and optional
  private payloads. Do not introduce a required `extension.json` sidecar.
- [ ] Do not ship `pj-plugin-check` inside each plugin ZIP; it is release/runtime
  infrastructure supplied by its owning repository.

### Enforce immutable identity and provenance

- [ ] Add `min_sdk_required` to `MANIFEST_FIELDS` in
  `scripts/submit_to_registry.py` and propagate the validated value into the
  registry submission.
- [ ] Remove any `--skip-checksum-verify` path from registry submission.
- [ ] Make the release/submission tooling reject an existing `(plugin id,
  version)` even if the new artifact appears otherwise valid. The registry must
  never associate a second digest with that pair.
- [ ] Require strict SemVer for public plugin versions.
- [ ] Verify that the embedded manifest, packaged manifest, release tag, ZIP
  name, and registry record all identify the same plugin version.

### Preserve the module boundary

- [ ] Admit exactly one public marketplace module per DSO.
- [ ] Keep an optional configuration dialog as a facet of that same module. Do
  not split a plugin into runtime and dialog DSOs merely to separate PJ4's
  headless loader from its GUI.
- [ ] Ensure plugins never write mutable data beside their DSO.
- [ ] Resolve bundled resources and private dependencies relative to the actual
  DSO location, never the current working directory or a human-readable install
  folder.

## P0 — Make the ROS 2 proxy safe to preflight

The root ROS 2 proxy is intentionally discoverable and installable on a machine
without ROS. Its current creation path loads the distro-specific private DSO,
which prevents a universal create-bind-destroy admission check.

- [ ] Write a helper-level test that validates the root ROS 2 proxy on a machine
  with no ROS installation and no ROS environment configured.
- [ ] Write lifecycle tests for destroy-before-start, repeated failed start,
  unsupported ROS distro, missing private DSO, and successful retry after a
  recoverable configuration error.
- [ ] Refactor `proxyCreate()` to allocate only a lightweight proxy context.
- [ ] Make proxy bind store the host registry without detecting ROS or loading a
  private implementation DSO.
- [ ] Retain configuration in the proxy until runtime activation.
- [ ] Load, create, and bind the selected distro implementation only from
  `start()` or an explicitly ROS-dependent dialog action.
- [ ] Make destroy correct whether the private implementation was never loaded,
  partially initialized, running, stopped, or failed.
- [ ] Treat nested `dist/<distro>/...` DSOs as private implementation payloads:
  hash and retain them as package content, but do not expose them as independent
  marketplace modules.
- [ ] Keep the existing DSO-relative lookup for those private payloads so it
  works from content-addressed store paths.

## P1 — Harden construction and manifest behavior

- [ ] Add a preflight test for `toolbox_mosaico` with historical settings
  present and no network service available.
- [ ] Defer Mosaico worker-thread creation and automatic connection from dialog
  construction/bind until explicit runtime or dialog activation. A helper
  preflight must be deterministic and free of background network work.
- [ ] Pass an explicit manifest to the dialog plugin declaration for maintained
  plugins that currently rely on construction fallback, including:
  - `data_load_blf`
  - `data_load_mf4`
  - `toolbox_data_exporter`
- [ ] Add a test that a dialog facet and its runtime facet report the same module
  identity.
- [ ] Retain generated `.pjmanifest.json` files only as development/tooling
  aids. The managed runtime and active profile must ignore them.
- [ ] Correct stale CMake comments that claim runtime discovery consumes those
  generated sidecars.
- [ ] Do not remove `pj_emit_plugin_manifest` blindly: it also configures symbol
  visibility and, on applicable platforms, linker hardening such as
  `-Bsymbolic-functions`.

## P2 — Prepare for SDK 1.0 cleanup

- [ ] Keep the deprecated SDK 0.20 direct-C++ parser bridge during the 0.21
  migration so already-published binaries remain loadable.
- [ ] Before SDK 1.0, require all official parser tests to use only the C ABI and
  functional handles; no test may cast an opaque context to a C++ plugin base.
- [ ] Remove the deprecated direct-C++ bridge only in SDK 1.0, after the
  official parsers and representative external plugins have completed the
  migration.

## TDD exit gates

The implementation order for every section above is: commit a failing
regression/contract test, make the smallest implementation change that passes
it, then refactor without weakening the test.

- [ ] **SDK 0.21 gate:** all four official parser families pass functional
  scalar/object tests as applicable, legacy parity tests, malformed-input tests,
  and lifecycle tests on all supported platforms.
- [ ] **Artifact gate:** every final release ZIP passes the canonical helper in
  a fresh process and the registry accepts only its unique immutable identity.
- [ ] **ROS 2 gate:** the public proxy passes admission without ROS installed and
  defers all distro-specific loading until explicit activation.
- [ ] **Loader-state gate:** validating one plugin cannot make a later
  validation pass because symbols remain loaded from the earlier DSO.
- [ ] **Compatibility gate:** an existing SDK 0.20 parser artifact remains
  loadable by PJ4 through the deprecated bridge, while newly released official
  parsers use the functional C API.

## Cross-repository ownership

| Repository | Owns |
| --- | --- |
| `plotjuggler_sdk` | SDK 0.21 functional parser ABI, compatibility bridge, canonical C extension behavior, and eventual SDK 1.0 removal policy |
| `pj-official-plugins` | Parser migration and tests, plugin manifests, artifact construction, final-ZIP admission in release CI, ROS 2 proxy lifecycle, and plugin-local lifecycle hardening |
| `PJ4` | `pj-plugin-check`, headless in-process runtime/loader, immutable profiles, local-ZIP admission, bundled-plugin seeding/indexing, and shipping required helper processes in AppImage, Debian, and Windows packages |
| Marketplace registry | Immutable `(id, version) -> digest` records, schema validation, compatibility metadata, and rejection of version reuse |

## Behavior that must remain unchanged

- Managed local ZIP installation remains a fully supported marketplace feature
  and follows the same helper admission and immutable-profile path as registry
  artifacts.
- `--plugin-dir` remains a developer override with strict priority. PJ4 loads
  plugins from the path supplied by the developer and never copies them into a
  managed store or another folder.
- Bundled/default plugins remain the startup kit for Debian, AppImage, and other
  installers. PJ4 seeds the managed store from the exact validated bundled
  artifacts and records their digests; plugins do not seed themselves.
- Fresh, fully validated installations may become active immediately.
  Failed upgrades retain only the previously validated active profile; the
  registry never reuses the rejected candidate's version.
- Plugin runtime code remains in-process. Full process isolation, worker/RPC
  plugin execution, and a separate GUI-plugin architecture are not goals of
  this plan.
