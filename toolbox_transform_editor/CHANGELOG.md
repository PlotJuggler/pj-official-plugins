# Changelog — toolbox_transform_editor

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_transform_editor/`.

## [1.2.0] - Unreleased

### Added
- Object inputs. Drop point clouds, images, scenes or any other object topic next to the series, or
  pick one from the list under the inputs table and press "Add input" (the list names each type in
  words: point cloud, image, image annotations, scene, transforms). The table shows Input, Var and
  Type. Each input is a variable of the script: Var defaults to the leaf of the topic (`/lidar_top`
  becomes `lidar_top`, `pose/x` becomes `x`, a repeat gets `_2`, a Lua or Python keyword gets a
  trailing `_`), the function header reads `function( lidar_top, x )`, and `inputs["/lidar_top"]`
  keeps working. With series only nothing changes: `time`, `value` and `v1..vN`, so every
  existing function and transform still works. Var cannot be edited in the table (the dialog has
  no cell editing); the library's Use renames it.
- No outputs to declare. Run the script and the editor learns what it returns: `return value` is
  one series, `return {value = ..., cropped = ...}` is a series and a point cloud. The name is asked
  when you press "Create...", in a small dialog that says what will be created ("series `max_z`,
  point cloud `cropped`; computed per /lidar_top frame"). A name cannot be empty, be one of the
  inputs or contain `__`; one that already belongs to a recipe made by this editor asks for a second
  OK to replace it. Opened from Custom Topics, the button reads "Modify" and the name stays.
- One preview. A recipe with object inputs is run 300 ms after you stop editing and again, at most
  twice a second, as the cursor moves. The status line under the preview shows the result
  (for example `max_z: 1.83` and `cropped: point cloud, 23 144 points`, joined by a middle dot), "unavailable (reason)" for a value the
  script could not produce, or the host's error. Number outputs are plotted when the host provides
  their series over the whole recording and shown as a readout at the cursor otherwise; object
  outputs show in the "Transform Editor preview" scene tab. When the cursor is before the first
  sample of the first object input, the preview runs at that first sample and says so.
- Create is enabled with at least one input, a function body and a run that succeeded with every
  output typed; otherwise the status line says why (and so does the button's tooltip on hosts that
  show it).
- Python for objects. A recipe with object inputs runs in the selected language: a Python body is
  wrapped in `def evaluate(inputs, params):` with the same variables.
- Advanced (collapsed): the params JSON object handed to the script as `params` and "Pin at current
  time", for recipes with object inputs.
- The Function Library has a Kind column (Series, 3D, 2D), shows each function's description and
  required inputs, and Use on an object function names the Var of the first matching unbound
  input after the function's variables ("needs: cloud (point cloud)" when there is none). New
  built-in object functions: `points_per_frame`, `lidar_crop`, `lidar_crop_map`, `witness_of_crop`,
  `cam_threshold`, `cam_annotations` and `depth_cloud`. Libraries saved by older versions keep
  loading; the new fields are optional.
- After creating a recipe with object outputs, a "Show in 3D" / "Show in 2D" button opens a scene tab
  with them (hosts with scene tabs only).
- The recipe's input resolution and script are built by the same shared helpers the assistant uses
  (`common/derived_recipes`), so the preview runs what Create installs and dataset-qualified inputs
  resolve identically.
- Older saved states keep loading: states with an explicit kind and declared outputs (the outputs
  are only a hint; the run infers them again) and states with `-- pj-kind: on_demand` /
  `-- pj-outputs:` / `-- pj-params:` header lines in the global code.

### Changed
- A series function that returns several values (`return a, b`) creates `name/a`, `name/b`; the
  count is read from the return statements. States saved with a comma-separated name keep their
  names.
- A recipe with object inputs keeps its saved editor state in its params under `"__editor"` next to
  the user's own params (transforms keep today's format), and is installed with the outputs the
  run inferred, bound by name.
- Poll previews once per GUI tick without sleeping; cancel and release pending previews when
  the script changes, expires, or the editor closes.
- Object inputs need a host with the SDK 0.36 surfaces (`create_data_processor_v2`,
  `submit_evaluation`, catalog snapshot v2, inferred outputs); on an older host the editor says so,
  Create stays disabled for them and the numeric transform works as before.

## [1.0.4] - 2026-08-04
