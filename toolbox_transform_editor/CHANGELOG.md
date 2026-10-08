# Changelog — toolbox_transform_editor

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_transform_editor/`.

## [1.2.0] - 2026-10-05

One editor for series and objects.

### Added
- Inputs by drag and drop. Drop series and object topics (point clouds, images, image annotations,
  scenes, transforms) from the Datasets tree; the table shows each input, its Var and its type in
  words. Each input is a variable of the script. Var defaults to the leaf of the topic (`/lidar_top`
  becomes `lidar_top`, `pose/x` becomes `x`, a repeat gets `_2`, a keyword gets a trailing `_`), and
  `inputs["/lidar_top"]` keeps working. With series only, the variables stay `time`, `value` and
  `v1..vN`. While the body is still the default `return value`, adding the first object input
  rewrites it to `return <first var>`; an edited body is never touched.
- Outputs are inferred. Run the script and the editor learns what it returns: `return value` is one
  series, `return {value = ..., cropped = ...}` is a series and a point cloud. The name is asked
  when you press "Create...", in a dialog that says what will be created ("series `max_z`, point
  cloud `cropped`; computed per /lidar_top frame"). A name cannot be empty, be one of the inputs or
  contain `__`; one that already belongs to a recipe of this editor asks for a second OK to replace
  it. Opened from Custom Topics, the button reads "Modify" and the name stays.
- Luau or Python for recipes with object inputs; a Python body is wrapped in
  `def evaluate(inputs, params):` with the same variables.
- One preview. A recipe with object inputs runs 300 ms after you stop editing and then, at most
  twice a second, as the cursor moves. The status line shows the result (`max_z: 1.83`,
  `cropped: point cloud, 23 144 points`), "unavailable (reason)" for a value the script could not
  produce, or the error the host reports. Number outputs are plotted as series curves over the
  whole recording when the host provides them, and shown as a readout at the cursor otherwise. While
  the host is still computing a curve the status line says "computing the series… N rows" and the
  previous curve stays until the new one has data.
  Object outputs show in a 3D or 2D scene view, embedded in the editor, that follows the cursor
  (a mix of 2D and 3D shows as 3D); a host that cannot embed a scene view shows only the status
  line. When the cursor is before the first instant at which every object input has a sample, the
  preview runs at that instant and says so in the timeline's seconds ("No sample of /lidar_top at
  the cursor (0.000 s): move the timeline to preview"); Create stays enabled. A script that returns nothing is reported with the names it can
  read (`The script returned no values · inputs are: lidar_top`). The preview recipes belong to the
  editor's panel and the host removes them when it closes.
- Create is enabled with at least one input, a function body and a run that succeeded; otherwise
  the status line, and the button's tooltip, say why.
- Advanced (collapsed): the params JSON object handed to the script as `params`, and "Pin at current
  time".
- The Function Library has a Kind column (Series, 3D, 2D), shows each function's description and
  required inputs, and Use on an object function rewrites its text to read the Var of the first
  matching unbound input and leaves the Vars as they are ("needs: cloud (point cloud)" when there
  is none). New built-in object functions:
  `points_per_frame`, `lidar_crop`, `lidar_crop_map`, `witness_of_crop`, `cam_threshold`,
  `cam_annotations` and `depth_cloud`.
- The time-series preview zooms (wheel, rectangle) and pans (Ctrl or middle drag); right-click
  undoes a rectangle zoom, and the wheel does not zoom out past the whole curve.
- Vars can be renamed: double-click an input row, type a name and press OK. A name must be an
  identifier, not a keyword or a name the script already owns (`inputs`, `params`, `math`...), and
  not the Var of another input; an empty name gives back the default. The script is never edited.
  The names are saved with the recipe.
- Errors name your own code: a host message at `script:8:` is shown as `line 1` (`globals line 2`
  for the globals pane), and a script that reads a name that is not bound ("attempt to index nil",
  "NameError") lists the Vars it can read. An error shown over the preview stays until the next
  result replaces it.
- When the host refuses the live preview recipe, or reports it in error or missing an input, the
  reason is shown over the preview as "Preview: ..." and stays until the recipe is accepted or
  healthy again. A Create the host refuses is shown over the preview too, until you edit the form.
- While the host computes the preview's series, the status line reads "Computing series: 3 / 10"
  on a host that reports its progress.

### Fixed
- Renaming a Var also renames it after a Luau concatenation (`"n=" .. cloud`); a field of the same
  name (`x.cloud`) is still left alone.

### Changed
- The Help is rewritten: inputs, return values, preview, Create and Modify, the Function Library,
  params and pin, series functions, what objects offer (fields, methods, `pj`), Python and the
  messages. Each part links to the online guide.
- A series function that returns several values (`return a, b`) creates `name/a`, `name/b`; the
  count is read from the return statements.
- The Custom Topics "+" button and the pencil on a recipe made here open this editor.
- Object inputs need a host with SDK 0.36 or newer; on an older host the editor says so, Create
  stays disabled for them and the series transform works as before.

## [1.0.4] - 2026-08-04
