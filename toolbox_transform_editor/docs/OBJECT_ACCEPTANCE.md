# Object recipes: native acceptance

Run the native app in an isolated display (not the one you work on) and load a point-cloud
dataset. The steps use `/lidar_top`; replace it with a topic of yours. Open the Transform Editor
from the "+" button of Custom Topics, on the Single Function tab.

1. **Inputs.** Drag the `/lidar_top` row from the Datasets tree into Inputs. Dragging is the only way
   to add an input: there is no topic picker and no Add button. The row shows the topic, its Var
   (`lidar_top`) and its type (`point cloud`). While the function body is still the default, it
   becomes `return lidar_top`. There is no outputs table and no engine to choose: the editor works
   the outputs out by running the function.

2. **Function.** Replace the body with (`inputs["/lidar_top"]` reads the same input as `lidar_top`):

   ```lua
   local c = lidar_top:crop_box{min={-15,-15,-1},max={15,15,1}}
   return {cropped=c, count=c:count()}
   ```

3. **Preview.** Stop typing: after 300 ms the function runs at the timeline cursor. Until a run has
   succeeded, Create... is disabled, and the status line (and the button's tooltip) says why. Once
   it has, the status line names both outputs (`count: N · cropped: point cloud, M points`),
   `count` is drawn as a curve over the whole recording and the cropped cloud shows in the scene
   view embedded in the editor. Move the cursor: the function runs again, at most twice a second.
   Put the cursor before the first frame of the topic: the status line says "No sample of
   /lidar_top at the cursor ... move the timeline to preview", and Create... stays enabled.

4. **Cancelling.** Edit the script while a run is pending: the run in flight is dropped and a new one
   starts after the pause. Close the editor with an expensive preview pending: the app stays
   responsive, and the host removes the preview recipe with the panel.

5. **Create.** Press Create.... A prompt says what will be created ("point cloud `cropped`, series
   `count`; computed per /lidar_top frame") and asks for a name, prefilled with the first output.
   An empty name, `/lidar_top` (an input path) or a name containing `__` reopens the prompt with the
   reason. Enter a name and press OK: the recipe is installed as the editor's own, into the undo
   history like any transform of yours, and the editor closes. Its outputs are listed in Custom
   Topics under that name. Creating another recipe under the same name asks for a second OK before
   it replaces this one.

6. **See it.** Open a 3D tab ("+", then the cube icon) and drag the cropped cloud's row from Custom
   Topics into it.

7. **Modify.** Select the recipe's row in Custom Topics and press the pencil: the editor reopens with
   its inputs, Vars and script. The button reads Modify and asks for no name. The outputs are
   inferred again by a new run, so Modify is enabled once it succeeds.
