# WebRTC demo / test rig

A dependency-light, browser-free rig to exercise `data_stream_webrtc` end to end:
a GStreamer `webrtcbin` sends `/dev/video0` (H.264) to the plugin, which renders
as live `PJ.VideoFrame` in a PlotJuggler 4 2D dock.

The plugin is the **answerer**; the GStreamer pipeline is the **offerer**. They
meet at the broker inside `stream_server.py`, which speaks the GStreamer
`webrtc_sendrecv` "simple signaling" protocol (`HELLO`/`SESSION` then verbatim
SDP/ICE relay) and auto-launches the camera sender (`send_camera.py`).

## Dependencies (Debian/Ubuntu)

```bash
# Signaling (both server and sender):
pip install websockets

# Sender (GStreamer webrtcbin + v4l2 + payloader + ICE):
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

## Quick start (one command)

```bash
pip install websockets        # one-time
python3 stream_server.py      # broker + auto-launched camera sender
```

Then in PlotJuggler 4: **Streaming → "WebRTC Video Client"**, set Address `127.0.0.1`,
Port `8443`, Our peer id `receiver`, Peer id empty, **Start**, and drop the
`webrtc/video` topic into a 2D dock. `stream_server.py` waits for the source to
register, launches the camera sender for you, and re-launches it if you Stop/Start —
no start-order, no extra terminals. Options: `--encoder vaapi|nvenc|jetson|passthrough`,
`--device /dev/videoN`, `--port N`.

The sender runs as a subprocess on purpose: driving GStreamer from the same asyncio
loop that hosts the websocket server segfaults on some PyGObject stacks, so the
broker launches the standalone `send_camera.py` in its own process.

Live `/dev/video0` renders in the dock and follows the live tip. On a LAN this
works STUN-less via host ICE candidates (the laptop runs the sender and the PJ4
receiver simultaneously over localhost).

## Files

- `stream_server.py` — broker + sender supervisor; the one command you run.
- `send_camera.py` — the GStreamer `webrtcbin` offerer, launched by
  `stream_server.py`. Runnable standalone:
  `python3 send_camera.py --server ws://HOST:PORT --device /dev/videoN --peer receiver`.
- `recv_probe.cpp` — headless C++ answerer that links the plugin cores to confirm
  decode without PlotJuggler.

## Encoder selection

`--encoder` picks the encode element so the same rig covers every target:
`x264` (software), `vaapi` (Intel `vah264enc`/`vaapih264enc`), `nvenc`
(NVIDIA `nvh264enc`), `jetson` (`nvv4l2h264enc`), or `passthrough` (camera
already emits H.264 — no transcode).

## Troubleshooting

- **No video / ICE stalls on an offline LAN.** `webrtcbin` defaults to a Google
  STUN server; the DNS lookup can hang offline. Rely on host candidates, or set
  STUN/TURN on both ends. Add TURN (with creds) in the plugin's ICE table when
  the peer is behind NAT/cellular.
- **`Gst.parse_launch` raises.** The selected encoder element is missing /
  driver not present. Try `--encoder x264`.
- **Green/garbage until the first keyframe.** Ensure `config-interval=-1` (it is
  here) and a frequent `key-int-max` so SPS/PPS repeat with every IDR.
- **`passthrough` fails.** Many USB cams emit MJPEG, not H.264. Use `x264`/`vaapi`.
- **`set-remote-description` fails on the answer.** Older GStreamer bindings use
  `GstSdp.sdp_message_new` + `sdp_message_parse_buffer` instead of
  `SDPMessage.new_from_text`; switch if needed.
