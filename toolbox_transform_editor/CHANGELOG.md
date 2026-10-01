# Changelog — toolbox_transform_editor

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_transform_editor/`.

## [1.2.0] - Unreleased

### Added
- One generic editor: inputs, a script, outputs. The inputs table takes series and object
  topics (an object-topic picker lists them from the host catalog; a Type column says which
  kind each input is) and the outputs table declares each output's name and type (number,
  string, kPointCloud, kSceneEntities, kImageAnnotations, kImage). There is no engine to
  choose: with only numeric inputs and outputs the editor builds the per-sample transform it
  always built ("Computed per sample"); any object input or output makes it a recipe evaluated
  where a consumer asks, at the playhead ("Computed at the cursor"), re-deduced on every change
  of the inputs or outputs. 2D/3D objects (point clouds, scene entities, image annotations,
  images) are now CREATED and MODIFIED from the editor, not only previewed. The editor modifies
  only recipes it created itself; recipes of the assistant or of other plugins are never loaded
  into it.
- The first object input added to an untouched form retypes the default output to the same
  kind of object (point cloud in, point cloud out); numeric inputs keep a number.
- The preview follows the output. Numeric outputs keep the plot preview. Object outputs are
  previewed in a scene tab ("Transform Editor preview", 3D or 2D by output type) fed by a
  temporary recipe that follows the playhead; it is replaced when the form changes and removed,
  with its tab, when the editor closes or on Create. Hosts without scene tabs show the text
  summary only.
- The result pane is a readable summary of the evaluation (for example "cloud: point cloud,
  23 144 points, bounds x[-10,10] y[-10,10] z[-2,3]") or the error text, instead of raw JSON.
- A params JSON field and a "Pin at current time" checkbox appear for recipes evaluated at the
  cursor; the latter keeps one evaluation at the playhead.
- After creating such a recipe a "Show in 3D" / "Show in 2D" button opens a scene tab with its
  object outputs (hosts with scene tabs only).
- The recipe's input resolution and script are built by the same shared helpers
  the assistant uses (`common/derived_recipes`), so the preview runs what Create
  installs and dataset-qualified inputs resolve identically.
- Older saved states with `-- pj-kind: on_demand` / `-- pj-outputs:` /
  `-- pj-params:` header lines in the global code still load, into the form, and so do
  states saved with an explicit kind: the saved kind is a hint kept until the inputs or
  outputs change, then the engine is deduced again.

### Changed
- Neutral wording: "Inputs" / "Outputs" headers, no "timeseries" placeholder in the inputs
  table, the function header reads `function( inputs, params )` for recipes evaluated at the
  cursor (it was cut off as "on-demand chunk( inputs, params )"), and the Create button reads
  "Create Derived Object" / "Modify Derived Object" for them.
- A recipe evaluated at the cursor keeps its saved editor state in its params under
  `"__editor"` next to the user's own params (transforms keep today's format).
- Poll previews once per GUI tick without sleeping; cancel and release pending previews when
  the script changes, expires, or the editor closes.
- Object inputs and outputs need a host with the SDK 0.36 surfaces
  (`create_data_processor_v2`, `submit_evaluation`, catalog snapshot v2); on an older host the
  editor says so, Create stays disabled for them and the numeric transform works as before.

## [1.0.4] - 2026-08-04
