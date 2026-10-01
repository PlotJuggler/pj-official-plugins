# Changelog — toolbox_transform_editor

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_transform_editor/`.

## [1.2.0] - Unreleased

### Added
- Single Function tab: an explicit kind selector (Transform | On-demand) next to
  the Lua/Python buttons. On-demand recipes produce 3D/2D objects (point clouds,
  scene entities, image annotations, images) and numbers, evaluated where a
  consumer asks, and are now CREATED and MODIFIED from the editor, not only
  previewed. The editor modifies only recipes it created itself; recipes of the
  assistant or of other plugins are never loaded into it.
- On-demand form: the inputs table also takes object topics (an object-topic
  picker lists them from the host catalog; a Type column says which kind each
  input is), an outputs table (name + type: number, string, kPointCloud,
  kSceneEntities, kImageAnnotations, kImage), a params JSON field and a
  "Pin at current time" checkbox that keeps one evaluation at the playhead.
- After creating an on-demand recipe a "Show in 3D" / "Show in 2D" button opens
  a scene tab with its object outputs (hosts with scene tabs only).
- The recipe's input resolution and script are built by the same shared helpers
  the assistant uses (`common/derived_recipes`), so the preview runs what Create
  installs and dataset-qualified inputs resolve identically.
- Older saved states with `-- pj-kind: on_demand` / `-- pj-outputs:` /
  `-- pj-params:` header lines in the global code still load, into the form.

### Changed
- An on-demand recipe's saved editor state lives in its params under
  `"__editor"` next to the user's own params (transforms keep today's format).
- Show on-demand JSON reports in the main preview pane. Poll on-demand previews
  once per GUI tick without sleeping; cancel and release pending previews when
  the script changes, expires, or the editor closes.
- The on-demand kind needs a host with the SDK 0.36 surfaces
  (`create_data_processor_v2`, `submit_evaluation`, catalog snapshot v2); on an
  older host its button is disabled and the transform kind works as before.

## [1.0.4] - 2026-08-04
