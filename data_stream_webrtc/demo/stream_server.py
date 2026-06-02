#!/usr/bin/env python3
"""One-command WebRTC streaming server for the PJ4 data_stream_webrtc plugin.

Runs the GStreamer "simple-signaling" broker and auto-launches the camera sender
(send_camera.py) as a subprocess whenever a receiver registers, so the demo is a
single command with no start-order to get wrong:

    python3 stream_server.py                       # x264, /dev/video0, :8443
    python3 stream_server.py --encoder vaapi --device /dev/video2

Start this once, then Start the WebRTC source in PlotJuggler (peer id 'receiver').
The server waits for that receiver, spawns the sender, and re-spawns it if you
Stop/Start the source. The sender runs in its OWN process (send_camera.py) on
purpose: driving GStreamer from the asyncio loop that also hosts the websocket
server segfaults on some PyGObject stacks. The plugin is the ANSWERER;
send_camera.py is the OFFERER.

Deps: websockets (this broker) + send_camera.py's deps (python3-gi, GStreamer).
"""
import argparse
import asyncio
import logging
import os
import sys

import websockets

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
log = logging.getLogger("stream-server")

SEND_CAMERA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "send_camera.py")


class Broker:
    """GStreamer 'simple signaling' relay (HELLO/SESSION + verbatim SDP/ICE),
    plus a `receiver_present` event the sender supervisor waits on."""

    def __init__(self, receiver_id):
        self.receiver_id = receiver_id
        self.peers = {}     # uid -> [ws, status]   status: None | 'session'
        self.sessions = {}  # uid -> paired uid
        self.receiver_present = asyncio.Event()

    async def hello(self, ws):
        msg = await ws.recv()
        parts = msg.split(maxsplit=1) if isinstance(msg, str) and msg.startswith("HELLO") else []
        uid = parts[1] if len(parts) == 2 else ""
        if not uid or " " in uid or uid in self.peers:
            await ws.close(code=1002, reason="invalid peer uid")
            raise websockets.ConnectionClosed(None, None)
        self.peers[uid] = [ws, None]
        await ws.send("HELLO")
        log.info("registered peer %r", uid)
        if uid == self.receiver_id:
            self.receiver_present.set()
        return uid

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

    async def relay_or_command(self, uid, msg):
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


async def _wait_cleared(event):
    while event.is_set():
        await asyncio.sleep(0.2)


async def sender_supervisor(broker, url, our_id, peer_id, device, encoder):
    while True:
        await broker.receiver_present.wait()  # blocks until the plugin registers
        log.info("receiver %r present; launching camera sender (%s, %s)", peer_id, encoder, device)
        proc = await asyncio.create_subprocess_exec(
            sys.executable, SEND_CAMERA, "--server", url, "--our-id", our_id,
            "--peer", peer_id, "--encoder", encoder, "--device", device)
        # Run until the sender exits (the broker closes it when the receiver
        # leaves) or the receiver disconnects; then tear down and wait again.
        waiter = asyncio.ensure_future(proc.wait())
        gone = asyncio.ensure_future(_wait_cleared(broker.receiver_present))
        await asyncio.wait({waiter, gone}, return_when=asyncio.FIRST_COMPLETED)
        gone.cancel()
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=3)
            except asyncio.TimeoutError:
                proc.kill()
        log.info("camera sender stopped (code %s)", proc.returncode)
        await asyncio.sleep(0.5)  # backoff before re-launching to a still-present receiver


async def amain(args):
    broker = Broker(args.peer)
    url = "ws://127.0.0.1:{}".format(args.port)
    asyncio.get_running_loop().create_task(
        sender_supervisor(broker, url, args.our_id, args.peer, args.device, args.encoder))
    log.info("listening on ws://%s:%d — now Start the WebRTC source in PlotJuggler (peer id %r)",
             args.host, args.port, args.peer)
    async with websockets.serve(broker.handler, args.host, args.port, max_queue=16):
        await asyncio.Future()  # run forever


def main():
    ap = argparse.ArgumentParser(description="One-command WebRTC streaming server for PJ4.")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--device", default="/dev/video0")
    ap.add_argument("--encoder", default="x264",
                    choices=["x264", "vaapi", "nvenc", "jetson", "passthrough"])
    ap.add_argument("--peer", default="receiver", help="the plugin's 'Our peer id'")
    ap.add_argument("--our-id", default="sender")
    args = ap.parse_args()
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
