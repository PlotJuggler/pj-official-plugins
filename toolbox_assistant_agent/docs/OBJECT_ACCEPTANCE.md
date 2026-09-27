# Object tools: native acceptance

Run the native app in an isolated display with `ASSISTANT_FAKE_BACKEND=1` and
load a point-cloud dataset. This uses the real plugin, GUI executor and runtime
services without invoking a model or sending the dataset to a backend.

In Assistant Agent, send these scripted commands, replacing the topic and time
with values from `list topics` (times use the displayed axis):

1. `list topics`
2. `describe /lidar_top`
3. `crop /lidar_top at 1532402928.648`
4. `pin /lidar_top at 1532402928.648`
5. `status`
6. `show toolbox-assistant-agent/lidar_top_pin/cropped in 3d`

The evaluation and finding must return completed summaries with matching point
counts and bounds, not point buffers. Status must show one owned finding, its
bytes and readiness. The created scene must contain the named object and carry
the assistant ownership badge. Saving and reloading a layout must preserve the
pin instant, its recipe and the owned scene. A generic layout requires its input
dataset to be loaded before restoring it.

Open Transform Editor, drag the actual object row from the catalog into Inputs,
and put this in GLOBAL:

```lua
-- pj-kind: on_demand
-- pj-outputs: cropped:kPointCloud, count:number
-- pj-params: {}
```

Put this in FUNCTION:

```lua
local c = inputs["/lidar_top"]:crop_box{min={-15,-15,-1},max={15,15,1}}
return {cropped=c, count=c:count()}
```

The main preview pane must show JSON with completed outputs. This mode previews
only: the scalar Create button is disabled. Changing the script, switching tabs
or closing the editor cancels its pending evaluation and releases the handle.

## Verification recorded on 2026-09-27

The native app, current SDK 0.35 headers and actual plugin shared libraries were
exercised on Xephyr using `nuscenes-scene-0061.mcap`. The assistant crop and pin
returned 8,093 points; status reported one finding, 161,970 bytes and `ready`.
After restarting the app, loading the dataset and restoring the saved generic
layout, the same finding and bytes were present. The editor object drag and
header preview returned 10,605 points for the larger box above, with complete
coverage; closing an expensive pending preview left the app responsive and it
exited successfully.

This acceptance exposed two issues that fake service tests could not detect:
the scripted backend used named coordinate keys instead of positional arrays,
and the editor sent reports to a library-only widget absent from its main UI.
Both paths now use the actual binder contract and a visible main report pane.
The initial owned scene also exposed host-side camera framing and ownership
badge issues. After the host fixes, a fresh process and a newly created finding
produced a visible cloud immediately on attaching it to a new scene: the host
selected `lidar_top` automatically and displayed the `Assistant Agent` ownership
badge. No frame selection or camera adjustment was needed.

Deterministic tests separately cover pending/completed/failed/cancelled results,
expiry, abandoned callers, cancellation followed by a new turn, editor changes
and teardown, single installation while reading a finding asynchronously, own
finding accounting, and field tables for all eight supported object types.
