#!/usr/bin/env python3
"""One-command multi-camera WebRTC streaming server for the data_stream_webrtc plugin.

Runs the GStreamer "simple-signaling" broker AND owns the camera catalog. It

  1. advertises a multi-camera catalog (>=2 cameras: a real /dev/video0 plus a
     synthetic videotestsrc) to the receiver on HELLO and on a `list` request,
  2. accepts a `subscribe` naming the chosen streams, and
  3. launches a multi-track GStreamer offerer (send_cameras.py) that builds ONE
     webrtcbin with one H.264 sendonly track per requested camera, each m-line's
     `a=mid` rewritten to the camera's stream id (the mid == stream-id contract).

So the demo stays a single command with no start-order to get wrong:

    python3 stream_server.py                       # x264, :8443, legacy autostart
    python3 stream_server.py --encoder vaapi
    python3 stream_server.py --no-legacy-autostart # require a real `subscribe`

The sender always runs in its OWN process (send_cameras.py) on purpose: driving
GStreamer from the asyncio loop that also hosts the websocket server segfaults on
some PyGObject stacks. The plugin is the ANSWERER; send_cameras.py is the OFFERER.

Backward compatibility
----------------------
A receiver that does NOT advertise/subscribe (the legacy single-stream path, and
the plugin's current shipped behavior) simply never sends `subscribe`. With
`--legacy-autostart` (ON by default) the broker pre-arms a subscribe to every
advertised camera the moment such a receiver registers, so the zero-click demo
still streams. `--single` narrows that autostart to one camera to reproduce the
old single-stream demo exactly. A subscribe-capable receiver overrides the
autostart with its own selection.

Deps: websockets (this broker) + send_cameras.py's deps (python3-gi, GStreamer).
"""
import argparse
import asyncio
import json
import logging
import os
import re
import sys

import websockets

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
log = logging.getLogger("stream-server")

HERE = os.path.dirname(os.path.abspath(__file__))
SEND_CAMERAS = os.path.join(HERE, "send_cameras.py")

# The signaling protocol version this broker speaks (PROTOCOL.md). A subscribe
# with a higher `protocol` is rejected with `ERROR unsupported protocol <n>`.
PROTOCOL_VERSION = 1

# Advertised catalog, built at startup from the cameras ACTUALLY connected
# (detect_catalog below) — one entry per physical camera, generic names
# camera0/camera1/... `id == mid` is the contract; `device` is consumed only by
# the launched sender. Populated in amain() so --device can force the first one.
CAMERAS = []


def _v4l2_capture_devices():
    """Auto-detect real V4L2 capture cameras: one /dev/videoN per PHYSICAL camera.

    Each camera exposes several nodes (capture + metadata/control); we probe
    VIDIOC_QUERYCAP, keep only VIDEO_CAPTURE nodes, and de-dup by USB `bus_info`
    so a camera with nodes video0+video1 yields a single device (its first
    capture node). No external tools (v4l2-ctl) required."""
    import fcntl
    import glob
    import struct

    VIDIOC_QUERYCAP = 0x80685600  # _IOR('V', 0, struct v4l2_capability) — 104 bytes
    V4L2_CAP_VIDEO_CAPTURE = 0x00000001
    V4L2_CAP_DEVICE_CAPS = 0x80000000

    def _num(path):
        m = re.search(r"(\d+)$", path)
        return int(m.group(1)) if m else 0

    devices = []
    seen = set()
    for dev in sorted(glob.glob("/dev/video*"), key=_num):
        try:
            fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
        except OSError:
            continue
        try:
            cap = bytearray(104)
            try:
                fcntl.ioctl(fd, VIDIOC_QUERYCAP, cap, True)
            except OverflowError:  # older Pythons treat the ioctl nr as signed
                fcntl.ioctl(fd, VIDIOC_QUERYCAP - (1 << 32), cap, True)
            bus_info = cap[48:80].split(b"\0", 1)[0].decode("ascii", "replace")
            caps, device_caps = struct.unpack_from("<II", cap, 84)
            effective = device_caps if (caps & V4L2_CAP_DEVICE_CAPS) else caps
            if not (effective & V4L2_CAP_VIDEO_CAPTURE):
                continue
            key = bus_info or dev
            if key in seen:
                continue
            seen.add(key)
            devices.append(dev)
        except OSError:
            pass
        finally:
            os.close(fd)
    return devices


def detect_catalog(force_first_device=None):
    """Build the advertised catalog from the cameras actually connected. Generic
    names (camera0, camera1, ...) in detection order; `id == mid`. `--device`, if
    given, is forced as camera0. Falls back to one synthetic videotestsrc only if
    no real camera is found, so the demo still runs headless."""
    devices = _v4l2_capture_devices()
    if force_first_device:
        devices = [force_first_device] + [d for d in devices if d != force_first_device]
    cams = [
        {"id": "cam{}".format(i), "name": "camera{}".format(i), "codec": "h264",
         "width": 640, "height": 480, "source": "v4l2", "device": dev}
        for i, dev in enumerate(devices)
    ]
    if not cams:
        log.warning("no V4L2 capture camera found; advertising one synthetic videotestsrc as camera0")
        cams = [{"id": "cam0", "name": "camera0", "codec": "h264", "width": 640,
                 "height": 480, "source": "videotestsrc", "device": None}]
    return cams


def catalog_message():
    return json.dumps({
        "type": "catalog",
        "protocol": PROTOCOL_VERSION,
        "streams": [
            {"id": c["id"], "name": c["name"], "codec": c["codec"],
             "width": c["width"], "height": c["height"], "mid": c["id"]}
            for c in CAMERAS
        ],
    })


class Broker:
    """GStreamer 'simple signaling' relay (HELLO/SESSION + verbatim SDP/ICE),
    extended with catalog advertisement + subscribe bookkeeping, plus the
    `receiver_present` / `subscribe_ready` events the sender supervisor waits on."""

    def __init__(self, receiver_id):
        self.receiver_id = receiver_id
        self.peers = {}     # uid -> [ws, status]   status: None | 'session'
        self.sessions = {}  # uid -> paired uid
        self.receiver_present = asyncio.Event()
        self.subscribe_ready = asyncio.Event()
        self.catalog_ids = [c["id"] for c in CAMERAS]
        self.requested_streams = []  # set by `subscribe` (or by legacy autostart)
        # Monotonic counter bumped every time the well-known receiver registers.
        # Edge-triggered waiters compare against a captured value instead of
        # polling the level flag, so a fast Stop->Start (disconnect+reconnect
        # inside one poll window) cannot be missed (see wait_receiver_changed).
        self.receiver_generation = 0
        self.receiver_changed = asyncio.Event()
        # Monotonic counter bumped on every distinct subscription, so the sender
        # supervisor can detect a re-subscribe WHILE a sender is running and
        # relaunch with the new selection (PROTOCOL.md §5.1 — re-subscription is
        # supported; the demo restarts the pipeline).
        self.subscribe_generation = 0
        self.subscribe_changed = asyncio.Event()

    async def hello(self, ws):
        msg = await ws.recv()
        parts = msg.split(maxsplit=1) if isinstance(msg, str) and msg.startswith("HELLO") else []
        uid = parts[1] if len(parts) == 2 else ""
        if not uid or " " in uid or uid in self.peers:
            await ws.close(code=1002, reason="invalid peer uid")
            raise websockets.ConnectionClosed(None, None)
        # Register only AFTER the greeting sends succeed. If the socket drops
        # between recv and send, an early insert would orphan the uid in
        # self.peers (remove() is the only reaper), permanently blocking that id
        # — notably the well-known "receiver" — from reconnecting.
        self.peers[uid] = [ws, None]
        try:
            await ws.send("HELLO")
            if uid == self.receiver_id:
                # Push the catalog unsolicited so a discovery-capable receiver can
                # render it without a `list`. A legacy receiver ignores this JSON.
                await ws.send(catalog_message())
        except Exception:
            self.peers.pop(uid, None)
            raise
        log.info("registered peer %r", uid)
        if uid == self.receiver_id:
            self.receiver_generation += 1
            self.receiver_present.set()
            self._notify_receiver_changed()
        return uid

    def _notify_receiver_changed(self):
        # Pulse the change event so every waiter wakes, then immediately re-arm
        # it for the next transition (a fresh, unconsumed edge each time).
        self.receiver_changed.set()
        self.receiver_changed.clear()

    async def wait_receiver_changed(self, since_generation):
        """Return once the receiver state has advanced past `since_generation`
        (a new receiver registered) OR the receiver is currently absent. Misses
        no present->absent->present transition, unlike polling the level flag."""
        while self.receiver_generation == since_generation and self.receiver_present.is_set():
            await self.receiver_changed.wait()

    async def wait_subscribe_changed(self, since_generation):
        """Return once a new subscription has been set past `since_generation`."""
        while self.subscribe_generation == since_generation:
            await self.subscribe_changed.wait()

    async def start_session(self, uid, callee_id):
        if callee_id not in self.peers:
            await self.peers[uid][0].send("ERROR peer {!r} not found".format(callee_id))
            return
        if self.peers[uid][1] is not None or self.peers[callee_id][1] is not None:
            await self.peers[uid][0].send("ERROR peer busy")
            return
        await self.peers[uid][0].send("SESSION_OK")
        self.peers[uid][1] = self.peers[callee_id][1] = "session"
        self.sessions[uid], self.sessions[callee_id] = callee_id, uid
        log.info("session established: %r <-> %r", uid, callee_id)

    def set_subscription(self, ids):
        self.requested_streams = ids
        self.subscribe_generation += 1
        self.subscribe_ready.set()
        # Wake a running sender supervisor so a re-subscribe relaunches the
        # pipeline with the new selection (pulse-then-rearm, like receiver_changed).
        self.subscribe_changed.set()
        self.subscribe_changed.clear()

    async def handle_discovery(self, uid, obj):
        """Handle a catalog `list` / `subscribe` from a discovery-capable peer.
        Returns True if the message was a discovery command (and handled)."""
        mtype = obj.get("type")
        if mtype == "list":
            await self.peers[uid][0].send(catalog_message())
            return True
        if mtype == "subscribe":
            # Protocol-version negotiation (PROTOCOL.md §3.1): reject a subscribe
            # whose protocol is newer than we support. Absent == legacy/1.
            proto = obj.get("protocol", 1)
            if isinstance(proto, int) and proto > PROTOCOL_VERSION:
                await self.peers[uid][0].send("ERROR unsupported protocol {}".format(proto))
                return True
            ids = [s for s in obj.get("streams", []) if s in self.catalog_ids]
            if not ids:
                await self.peers[uid][0].send("ERROR no valid streams in subscribe")
                return True
            log.info("subscribe from %r: %s", uid, ids)
            self.set_subscription(ids)
            return True
        return False

    async def relay_or_command(self, uid, msg):
        # Intercept catalog discovery JSON (list/subscribe) BEFORE the session
        # relay, so it is handled with or without an active SESSION — the broker
        # owns the catalog, not the paired sender.
        if isinstance(msg, str) and msg.startswith("{"):
            try:
                obj = json.loads(msg)
            except Exception:
                obj = None
            if isinstance(obj, dict) and obj.get("type") in ("list", "subscribe"):
                await self.handle_discovery(uid, obj)
                return

        if self.peers[uid][1] == "session":
            await self.peers[self.sessions[uid]][0].send(msg)
            return
        if isinstance(msg, str) and msg.startswith("SESSION"):
            parts = msg.split(maxsplit=1)
            if len(parts) == 2:
                await self.start_session(uid, parts[1])
            else:
                await self.peers[uid][0].send("ERROR invalid SESSION command")
            return
        await self.peers[uid][0].send("ERROR not in a session")

    async def remove(self, uid):
        self.peers.pop(uid, None)
        other = self.sessions.pop(uid, None)
        if other is not None:
            self.sessions.pop(other, None)
            if other in self.peers:
                self.peers[other][1] = None
                try:
                    await self.peers[other][0].close(code=1001, reason="peer left session")
                except Exception:
                    pass
        if uid == self.receiver_id:
            self.receiver_present.clear()
            # A fresh receiver must re-subscribe (or re-trigger legacy autostart).
            self.subscribe_ready.clear()
            self.requested_streams = []
            self._notify_receiver_changed()
        log.info("removed peer %r", uid)

    async def handler(self, ws, *_unused):  # *_unused absorbs the legacy `path` arg
        uid = None
        try:
            uid = await self.hello(ws)
            async for msg in ws:
                await self.relay_or_command(uid, msg)
        except websockets.ConnectionClosed:
            pass
        finally:
            if uid is not None:
                await self.remove(uid)

    def streams_argv(self):
        """Encode the requested streams as send_cameras.py positional specs:
        `<id>=<source>:<device>`, in subscribe order."""
        by_id = {c["id"]: c for c in CAMERAS}
        out = []
        for sid in self.requested_streams:
            c = by_id[sid]
            dev = c["device"] or ""
            out.append("{}={}:{}".format(c["id"], c["source"], dev))
        return out


async def _wait_both(present, subscribe):
    await present.wait()
    await subscribe.wait()


async def legacy_autostart(broker, single):
    """Pre-arm a subscribe for a non-advertising receiver so the zero-click demo
    still streams. Re-arms each time a fresh receiver registers. A real
    `subscribe` from a discovery-capable receiver still wins: if it landed first,
    subscribe_ready is already set and this no-ops."""
    while True:
        try:
            await broker.receiver_present.wait()
            gen = broker.receiver_generation
            if not broker.subscribe_ready.is_set():
                ids = broker.catalog_ids[:1] if single else list(broker.catalog_ids)
                log.info("legacy autostart: no subscribe seen; defaulting to %s", ids)
                broker.set_subscription(ids)
            # Wait until THIS receiver leaves (or a fresh one registers) before
            # re-arming. Generation-based so a fast Stop->Start is not missed.
            await broker.wait_receiver_changed(gen)
        except Exception:
            # Never let a transient error kill auto-streaming for good; log and
            # loop. (asyncio would otherwise swallow the exception and stop the
            # task permanently.)
            log.exception("legacy_autostart iteration failed; continuing")
            await asyncio.sleep(0.5)


async def _terminate(proc):
    if proc.returncode is None:
        proc.terminate()
        try:
            await asyncio.wait_for(proc.wait(), timeout=3)
        except asyncio.TimeoutError:
            proc.kill()


async def sender_supervisor(broker, url, our_id, peer_id, encoder):
    while True:
        proc = None
        try:
            # Stream only once a receiver is present AND a selection exists (from a
            # real subscribe or the legacy autostart).
            await _wait_both(broker.receiver_present, broker.subscribe_ready)
            specs = broker.streams_argv()
            if not specs:
                broker.subscribe_ready.clear()
                continue
            sub_gen = broker.subscribe_generation
            recv_gen = broker.receiver_generation
            log.info("launching multi-cam sender for %s (%s)", broker.requested_streams, encoder)
            proc = await asyncio.create_subprocess_exec(
                sys.executable, SEND_CAMERAS, "--server", url, "--our-id", our_id,
                "--peer", peer_id, "--encoder", encoder, *specs)
            # Run until the sender exits, the receiver disconnects, OR the
            # selection changes (re-subscribe). Whichever fires first wins; a
            # re-subscribe relaunches with the fresh streams (PROTOCOL.md §5.1).
            waiter = asyncio.ensure_future(proc.wait())
            gone = asyncio.ensure_future(broker.wait_receiver_changed(recv_gen))
            resub = asyncio.ensure_future(broker.wait_subscribe_changed(sub_gen))
            await asyncio.wait({waiter, gone, resub}, return_when=asyncio.FIRST_COMPLETED)
            resubscribed = resub.done() and not waiter.done() and not gone.done()
            for t in (gone, resub):
                t.cancel()
            await _terminate(proc)
            log.info("camera sender stopped (code %s)", proc.returncode)
            if resubscribed:
                # New selection already armed (subscribe_ready set, generation
                # bumped): loop straight back and relaunch without clearing it.
                continue
            # Require a fresh subscribe (or autostart re-arm) before relaunching.
            broker.subscribe_ready.clear()
            await asyncio.sleep(0.5)
        except Exception:
            log.exception("sender_supervisor iteration failed; continuing")
            if proc is not None:
                await _terminate(proc)
            broker.subscribe_ready.clear()
            await asyncio.sleep(0.5)


async def amain(args):
    # Build the catalog from the cameras actually connected (--device, if given,
    # is forced as camera0).
    global CAMERAS
    CAMERAS = detect_catalog(args.device)
    broker = Broker(args.peer)
    url = "ws://127.0.0.1:{}".format(args.port)
    loop = asyncio.get_running_loop()
    loop.create_task(sender_supervisor(broker, url, args.our_id, args.peer, args.encoder))
    if args.legacy_autostart:
        loop.create_task(legacy_autostart(broker, args.single))
    log.info("listening on ws://%s:%d — now Start the WebRTC source in PlotJuggler (peer id %r)",
             args.host, args.port, args.peer)
    log.info("advertising %d camera(s): %s", len(CAMERAS),
             ", ".join("{}={}".format(c["name"], c["device"] or c["source"]) for c in CAMERAS))
    async with websockets.serve(broker.handler, args.host, args.port, max_queue=16):
        await asyncio.Future()  # run forever


def main():
    ap = argparse.ArgumentParser(description="One-command multi-camera WebRTC streaming server for PJ4.")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--device", default=None,
                    help="force a specific v4l2 device as the first camera (default: auto-detect all)")
    ap.add_argument("--encoder", default="x264",
                    choices=["x264", "vaapi", "nvenc", "jetson", "passthrough"])
    ap.add_argument("--peer", default="receiver", help="the plugin's 'Our peer id'")
    ap.add_argument("--our-id", default="sender")
    ap.add_argument("--legacy-autostart", dest="legacy_autostart", action="store_true", default=True,
                    help="auto-stream to a non-subscribing receiver (default: on)")
    ap.add_argument("--no-legacy-autostart", dest="legacy_autostart", action="store_false",
                    help="require a real `subscribe` before streaming")
    ap.add_argument("--single", action="store_true",
                    help="legacy autostart streams ONE camera (reproduces the old single-stream demo)")
    args = ap.parse_args()
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
