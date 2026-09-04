# WHEP demo / test rig

A dependency-light rig to exercise `data_stream_webrtc` end to end against
[mediamtx](https://github.com/bluenviron/mediamtx) (tested with v1.19.2) — the
reference WHEP server. mediamtx routes RTSP/RTMP/SRT/WHIP ingest straight to
WHEP egress **without transcoding**.

The plugin is a standard **WHEP client**: for every selected path it POSTs one
SDP offer to `http://<server>:8889/<path>/whep`, applies the answer, and
receives that camera's H.264 RTP on its own `PeerConnection` — one
`PJ.VideoFrame` topic `<prefix>/<path>` per camera.

## Quick start

1. **Get and run mediamtx** with the demo config (loopback-only by default):

   ```bash
   # download a release from https://github.com/bluenviron/mediamtx/releases
   ./mediamtx demo/mediamtx.yml
   ```

2. **Publish test cameras** over RTSP (needs `ffmpeg` OR `gst-launch-1.0`; no
   hardware required — both are synthetic test patterns):

   ```bash
   ./demo/publish_cameras.sh
   # or, to make cam0 a real webcam:
   ./demo/publish_cameras.sh <v4l2-device>   # e.g. the node from: v4l2-ctl --list-devices
   ```

3. **In PlotJuggler 4**: Streaming -> "WebRTC Video Client" -> Server URL
   `http://127.0.0.1:8889` -> Refresh list and tick `cam0`/`cam1` (found via
   the Control API), or type a path into "Manual path" and Add -> Start.

   Each selected path becomes topic `webrtc/<path>` (e.g. `webrtc/cam0`,
   `webrtc/cam1` with the default "webrtc" topic prefix). Drop each into a 2D
   dock to see it play.

## Headless probe (no PlotJuggler)

`webrtc_recv_probe` links the plugin's real WHEP/RTP/H.264 cores to confirm
decode without a running PlotJuggler instance:

```bash
./build/data_stream_webrtc/Release/data_stream_webrtc/webrtc_recv_probe \
    http://127.0.0.1:8889/cam0/whep --out /tmp/cam0.h264
# Ctrl-C to stop, then:
ffplay /tmp/cam0.h264
```

`--token T` sends `Authorization: Bearer T` if the server requires auth.

## Files

- `mediamtx.yml` — demo server config: RTSP ingest on `:8554`, WHEP egress on
  `:8889`, Control API on `:9997` (used for path discovery). All three are
  bound to `127.0.0.1` — loopback only. To expose to a LAN, change the
  `*Address` fields to `0.0.0.0:<port>` and configure `authInternalUsers`
  (mediamtx's auth mechanism) before doing so.
- `publish_cameras.sh` — publishes two H.264 test-pattern cameras (`cam0`,
  `cam1`) to mediamtx over RTSP using `ffmpeg` or `gstreamer`, whichever is
  found first. Pass a `/dev/videoN` path as `$1` to feed a real webcam into
  `cam0` instead. Supervises both publisher processes and exits/kills the
  sibling if either dies.
- `recv_probe.cpp` — headless WHEP client probe (`webrtc_recv_probe`), see
  above.

## Notes

- **Discovery.** The dialog's "Refresh list" polls the mediamtx Control API
  (`GET /v3/paths/list` on the Control API URL, default `:9997`). If the API
  is disabled or the server isn't mediamtx, type the path directly into
  "Manual path" and click Add — everything else works the same.
- **Auth.** The optional Bearer token field is sent on both the WHEP POST and
  the Control API request. Configure mediamtx's `authInternalUsers` to match
  whatever token you set.
- **H.264 only.** PlotJuggler's decoder is H.264-only, and mediamtx does not
  transcode, so whatever publishes into mediamtx must already emit H.264.
- **SPS/PPS.** In testing, mediamtx sends parameter sets in-band (in the RTP
  stream) rather than in the SDP answer's `sprop-parameter-sets`. The plugin
  handles both: in-band passthrough, with SDP-sprop priming as a fallback for
  servers that only advertise them there.
- **NAT/WAN.** For anything beyond a LAN/loopback, add STUN/TURN entries in
  the dialog's ICE Servers table — mediamtx and the plugin otherwise rely on
  host candidates.
- **Known WHEP-scope limitations** (future work, not bugs): no trickle-ICE via
  `PATCH`, no 406 counter-offer handling, no redirect following, no
  `Retry-After` parsing, an RFC3986-lite `Location` header resolution, and
  Bearer tokens are allowed over plain `http://` (fine on a LAN; prefer
  `https://` where the token actually matters).

## Troubleshooting

- **mediamtx fails to start with "no space left on device".** This is
  usually not disk space — it's `ENOSPC` from `inotify_add_watch` because the
  host's inotify watch limit is exhausted (e.g. a terminal/editor holding tens
  of thousands of watches). Either raise the limit:

  ```bash
  sudo sysctl fs.inotify.max_user_watches=524288
  ```

  or run mediamtx in a container instead, which gets its own inotify budget:

  ```bash
  docker run --network host bluenviron/mediamtx:1.19.2 /mediamtx.yml
  ```

  (mount `demo/mediamtx.yml` into the container at that path, or pass config
  via env vars per the mediamtx docs).
- **No video / ICE stalls.** Add STUN/TURN in the dialog's ICE Servers table,
  or make sure both PlotJuggler and mediamtx are reachable over loopback/LAN
  without a NAT in between.
- **Publisher connects but nothing decodes.** The publisher must emit H.264.
  Many USB webcams natively emit MJPEG — `publish_cameras.sh`'s ffmpeg/gst
  paths already transcode to `libx264`/`x264enc`, but a custom publisher must
  do the same.
- **"Refresh list" shows nothing.** The Control API may be disabled or
  unreachable (check the Control API URL field and that mediamtx has `api:
  yes`). Use "Manual path" instead — discovery is a convenience, not a
  requirement.
