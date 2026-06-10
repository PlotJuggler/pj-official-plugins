# WebRTC demo / test rig

A dependency-light, browser-free rig to exercise `data_stream_webrtc` end to end.
A GStreamer `webrtcbin` sends one or more H.264 cameras to the plugin, which
renders each as a live `PJ.VideoFrame` topic in a PlotJuggler 4 2D dock.

The plugin is the **answerer**; the GStreamer pipeline is the **offerer**. They
meet at the broker inside `stream_server.py`, which speaks the GStreamer
`webrtc_sendrecv` "simple signaling" protocol (`HELLO`/`SESSION` then verbatim
SDP/ICE relay), and additionally **owns a multi-camera catalog**: it advertises
the available cameras, accepts a `subscribe`, and launches the matching multi-
track sender.

## Multi-camera flow (catalog → subscribe → one webrtcbin, N tracks)

The broker advertises at least two cameras — a real `/dev/video0` (v4l2) and a
synthetic `videotestsrc` that needs no hardware:

```json
{ "type": "catalog", "protocol": 1, "streams": [
    {"id": "cam0", "name": "camera0", "codec": "h264", "width": 640, "height": 480, "mid": "cam0"},
    {"id": "cam1", "name": "camera1", "codec": "h264", "width": 640, "height": 480, "mid": "cam1"}
]}
```

It pushes this catalog unsolicited when a receiver registers, and re-sends it on
a `{"type":"list"}` request. A discovery-capable receiver selects cameras and
sends:

```json
{ "type": "subscribe", "protocol": 1, "streams": ["cam0", "cam1"] }
```

On `subscribe` the broker launches `send_cameras.py`, which builds **ONE**
`webrtcbin` with one H.264 **sendonly** track per requested camera and rewrites
the offer SDP so that **each m-line's `a=mid` equals its stream id** — the
`mid == stream-id` contract. The plugin reads `track->mid()` to route each track
to its own `PJ.VideoFrame` topic.

### `mid == stream-id`

For every video m-line, `a=mid:<x>` IS the stream id (== the camera id == the
`subscribe` token). `webrtcbin` assigns its own mids (`video0`, `video1`, …), so
`send_cameras.py` rewrites them — and the matching `a=msid:` and
`a=group:BUNDLE` lines — to the contract ids before putting the offer on the
wire. It calls `set-local-description` with the **original** offer and only the
**wire** SDP carries the rewritten ids.

### Legacy / single-stream compatibility

A receiver that does NOT advertise/subscribe (the original single-stream path)
just never sends `subscribe`. With `--legacy-autostart` (**on by default**) the
broker pre-arms a subscribe to every advertised camera the instant such a
receiver registers, so the zero-click demo still streams. `--single` narrows
that autostart to one camera to reproduce the old single-stream demo exactly.
`--no-legacy-autostart` requires a real `subscribe` before anything streams. The
single-camera legacy offerer `send_camera.py` is unchanged and remains the right
tool for stock `webrtc_sendrecv` peers and manual mode.

## Dependencies (Debian/Ubuntu)

```bash
# Signaling (both server and sender):
pip install websockets

# Sender (GStreamer webrtcbin + v4l2 / videotestsrc + payloader + ICE):
sudo apt install \
  python3-gi gir1.2-gst-plugins-bad-1.0 \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-nice

# Encoder backends (install the one you use):
#   x264   -> gstreamer1.0-plugins-ugly      (software fallback, default)
#   vaapi  -> gstreamer1.0-vaapi  OR the VA plugin in gstreamer1.0-plugins-bad (vah264enc)
#   nvenc  -> the nvcodec plugin (gstreamer1.0-plugins-bad built with NVENC)
#   jetson -> NVIDIA L4T multimedia plugins (nvv4l2h264enc)
```

These are **test-rig** dependencies only — the plugin itself needs none of them.
The synthetic `videotestsrc` camera needs no hardware, so the multi-camera path
is exercisable on any machine even without a webcam.

## Quick start (one command)

```bash
pip install websockets        # one-time
python3 stream_server.py      # broker + auto-launched multi-cam sender
```

Then in PlotJuggler 4: **Streaming → "WebRTC Video Client"**, set Address
`127.0.0.1`, Port `8443`, Our peer id `receiver`, **Start**. `stream_server.py`
waits for the source to register, advertises its cameras, and (with the default
`--legacy-autostart`) launches the multi-camera sender for you, re-launching it
if you Stop/Start — no start-order, no extra terminals. With a discovery-capable
build, pick the cameras you want in the dialog and only those are offered.

Drop each `webrtc/<camera name>` topic into a 2D dock (e.g. `webrtc/camera0`,
`webrtc/camera1`). Each subscribed camera is its own topic and follows the live tip.

Options: `--encoder vaapi|nvenc|jetson|passthrough`, `--device /dev/videoN` (the
v4l2 device for `cam0`), `--port N`, `--single`, `--no-legacy-autostart`.

The sender runs as a subprocess on purpose: driving GStreamer from the same
asyncio loop that hosts the websocket server segfaults on some PyGObject stacks,
so the broker launches the standalone sender in its own process. For the same
reason `gi` is imported (and versions required) **before** `websockets` in the
sender scripts — keep that import order.

On a LAN this works STUN-less via host ICE candidates (the laptop runs the
sender and the PJ4 receiver simultaneously over localhost).

## Files

- `stream_server.py` — broker + camera catalog + sender supervisor; the one
  command you run. Advertises cameras, handles `list`/`subscribe`, launches the
  matching multi-cam sender, and keeps the legacy single-stream demo working via
  `--legacy-autostart`.
- `send_cameras.py` — the multi-track GStreamer `webrtcbin` offerer. ONE
  `webrtcbin`, one H.264 sendonly track per requested camera, `a=mid` rewritten
  to the stream id. Launched by `stream_server.py`; runnable standalone:
  ```bash
  python3 send_cameras.py --server ws://HOST:PORT --peer receiver \
      cam0=v4l2:/dev/video0 cam1=videotestsrc:
  ```
  Cameras are positional `<id>=<source>:<device>` specs (`source` is `v4l2` or
  `videotestsrc`), in m-line order.
- `send_camera.py` — the original SINGLE-camera offerer (one m-line, GStreamer's
  auto mid `video0`). Kept for stock `webrtc_sendrecv` peers and the manual /
  legacy single-stream path. Runnable standalone:
  `python3 send_camera.py --server ws://HOST:PORT --device /dev/videoN --peer receiver`.
- `recv_probe.cpp` — headless C++ answerer that links the plugin cores to confirm
  decode without PlotJuggler.

## Encoder selection

`--encoder` picks the encode element so the same rig covers every target:
`x264` (software), `vaapi` (Intel `vah264enc`/`vaapih264enc`), `nvenc`
(NVIDIA `nvh264enc`), `jetson` (`nvv4l2h264enc`), or `passthrough` (camera
already emits H.264 — no transcode). Both senders share the same encoder chains.

## GStreamer multi-track caveats

1. **`webrtcbin` mids are read-only.** You cannot rename a transceiver's mid via
   the API, so `send_cameras.py` rewrites the **SDP text** (`a=mid` → stream id)
   before sending the offer.
2. **Pad order is determinism.** The i-th sink pad (`sendrecv.sink_<i>`) becomes
   the i-th m-line. The mid-rewrite keys on m-line order, so the positional spec
   order must stay stable end to end.
3. **`bundle-policy=max-bundle` is required.** All tracks ride one transport;
   ICE/DTLS happen once. Without it the answerer would have to negotiate N
   transports.
4. **Rewrite `a=group:BUNDLE` in lockstep.** After renaming mids, the BUNDLE
   group must list the new ids in the same order, or the SDP is inconsistent.
5. **Per-track `config-interval=-1`.** Each `rtph264pay` re-sends SPS/PPS with
   every IDR so a mid-stream join on any track can decode. Each m-line carries
   its own `sprop-parameter-sets`.
6. **`videotestsrc is-live=true` + distinct `pattern`.** `is-live=true` paces the
   synthetic source like a real camera; a per-camera `pattern` makes the streams
   visually distinguishable.
7. **Per-element encoder availability.** A missing encode element makes
   `Gst.parse_launch` raise. Fall back to `--encoder x264`.
8. **`set-local-description` uses the ORIGINAL offer.** Feed `webrtcbin`'s own
   SDP (internal mids) into `set-local-description`; only the SDP put on the wire
   carries the rewritten contract mids. Do NOT round-trip the rewritten text back
   into `webrtcbin`.

## Troubleshooting

- **No video / ICE stalls on an offline LAN.** `webrtcbin` defaults to a Google
  STUN server; the DNS lookup can hang offline. Rely on host candidates, or set
  STUN/TURN on both ends. Add TURN (with creds) in the plugin's ICE table when
  the peer is behind NAT/cellular.
- **`Gst.parse_launch` raises.** The selected encoder element is missing /
  driver not present. Try `--encoder x264`.
- **One camera shows, the other doesn't.** Check the second m-line negotiated:
  the synthetic `videotestsrc` camera (`cam1`/`webrtc/camera1`) needs no hardware,
  so if only it appears, your `/dev/video0` is missing or busy — pass
  `--device /dev/videoN` or subscribe to only the synthetic camera.
- **Green/garbage until the first keyframe.** Ensure `config-interval=-1` (it is
  here) and a frequent `key-int-max` so SPS/PPS repeat with every IDR.
- **`passthrough` fails.** Many USB cams emit MJPEG, not H.264. Use `x264`/`vaapi`.
- **`set-remote-description` fails on the answer.** Older GStreamer bindings use
  `GstSdp.sdp_message_new` + `sdp_message_parse_buffer` instead of
  `SDPMessage.new_from_text`; switch if needed.
