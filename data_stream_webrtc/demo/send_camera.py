#!/usr/bin/env python3
"""GStreamer webrtcbin camera sender (OFFERER) for the PJ4 WebRTC demo.

Drives a v4l2 camera into webrtcbin and negotiates against signaling_server.py.
This peer is ALWAYS the OFFERER: webrtcbin emits 'on-negotiation-needed' once
the pipeline is PLAYING, we create the offer, set the local description, and
send {"sdp":{"type":"offer",...}}. The plugin (libdatachannel) ANSWERS.

Options (see --help):
  --encoder  x264 (default) | vaapi | nvenc | jetson | passthrough
  --device   v4l2 device      (default /dev/video0)
  --server   signaling ws url  (default ws://127.0.0.1:8443)
  --our-id   our peer id       (default sender)
  --peer     id to SESSION to  (default receiver)  # the plugin's 'Our peer id'

Usually launched for you by stream_server.py; runnable standalone too.
Deps: python3-gi + GStreamer GI (gst-plugins-bad webrtcbin, nice, good, base)
      + websockets. See demo/README.md for the apt package list.
"""
import argparse
import asyncio
import json
import sys

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import Gst, GstWebRTC, GstSdp  # noqa: E402

import websockets  # noqa: E402

# Defaults; overridden by CLI flags in main().
DEVICE = "/dev/video0"
ENCODER = "x264"
SERVER = "ws://127.0.0.1:8443"
OUR_ID = "sender"
PEER_ID = "receiver"

# Every chain ends in H.264 elementary stream feeding the common payloader tail.
# config-interval=-1 re-sends SPS/PPS with every IDR (needed for mid-stream join).
RTP_TAIL = (
    "rtph264pay config-interval=-1 pt=96 ! "
    "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
    "webrtcbin name=sendrecv bundle-policy=max-bundle"
)


def encoder_chain(enc):
    if enc == "x264":
        return ("videoconvert ! "
                "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 ! "
                "video/x-h264,profile=constrained-baseline ! h264parse")
    if enc == "vaapi":
        if Gst.ElementFactory.find("vah264enc"):
            return "vapostproc ! vah264enc ! h264parse"
        return "vaapipostproc ! vaapih264enc ! h264parse"
    if enc == "nvenc":
        return "videoconvert ! nvh264enc ! h264parse"
    if enc == "jetson":
        return "nvvidconv ! nvv4l2h264enc insert-sps-pps=true ! h264parse"
    if enc == "passthrough":
        return "video/x-h264 ! h264parse"
    raise SystemExit("unknown ENCODER {!r}".format(enc))


def build_pipeline():
    desc = "v4l2src device={dev} ! {chain} ! {tail}".format(
        dev=DEVICE, chain=encoder_chain(ENCODER), tail=RTP_TAIL)
    print("PIPELINE:", desc, file=sys.stderr)
    return Gst.parse_launch(desc)


class Sender:
    def __init__(self, loop):
        self.loop = loop
        self.conn = None
        self.pipe = None
        self.webrtc = None

    def send(self, text):
        asyncio.run_coroutine_threadsafe(self.conn.send(text), self.loop)

    def on_negotiation_needed(self, element):
        promise = Gst.Promise.new_with_change_func(self.on_offer_created, element, None)
        element.emit("create-offer", None, promise)

    def on_offer_created(self, promise, element, _):
        promise.wait()
        reply = promise.get_reply()
        offer = reply.get_value("offer")
        p2 = Gst.Promise.new()
        element.emit("set-local-description", offer, p2)
        p2.interrupt()
        self.send(json.dumps({"sdp": {"type": "offer", "sdp": offer.sdp.as_text()}}))

    def on_ice_candidate(self, _element, mlineindex, candidate):
        self.send(json.dumps({"ice": {"candidate": candidate, "sdpMLineIndex": mlineindex}}))

    def start_pipeline(self):
        self.pipe = build_pipeline()
        self.webrtc = self.pipe.get_by_name("sendrecv")
        self.webrtc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self.on_ice_candidate)
        self.pipe.set_state(Gst.State.PLAYING)

    def handle_sdp(self, sdp_obj):
        assert sdp_obj["type"] == "answer", "sender is offerer; expected an answer"
        _res, sdpmsg = GstSdp.SDPMessage.new_from_text(sdp_obj["sdp"])
        answer = GstWebRTC.WebRTCSessionDescription.new(GstWebRTC.WebRTCSDPType.ANSWER, sdpmsg)
        promise = Gst.Promise.new()
        self.webrtc.emit("set-remote-description", answer, promise)
        promise.interrupt()

    def handle_ice(self, ice):
        self.webrtc.emit("add-ice-candidate", ice["sdpMLineIndex"], ice["candidate"])

    async def run(self):
        self.conn = await websockets.connect(SERVER)
        await self.conn.send("HELLO " + OUR_ID)
        assert await self.conn.recv() == "HELLO"
        await self.conn.send("SESSION " + PEER_ID)
        assert await self.conn.recv() == "SESSION_OK"
        self.start_pipeline()  # triggers on-negotiation-needed -> offer
        async for raw in self.conn:
            if raw.startswith("ERROR"):
                print("server error:", raw, file=sys.stderr)
                break
            msg = json.loads(raw)
            if "sdp" in msg:
                self.handle_sdp(msg["sdp"])
            elif "ice" in msg:
                self.handle_ice(msg["ice"])


def main():
    global DEVICE, ENCODER, SERVER, OUR_ID, PEER_ID
    ap = argparse.ArgumentParser(description="PJ4 WebRTC camera sender (offerer).")
    ap.add_argument("--server", default=SERVER, help="signaling ws url")
    ap.add_argument("--our-id", dest="our_id", default=OUR_ID)
    ap.add_argument("--peer", default=PEER_ID, help="peer id to SESSION to")
    ap.add_argument("--encoder", default=ENCODER,
                    choices=["x264", "vaapi", "nvenc", "jetson", "passthrough"])
    ap.add_argument("--device", default=DEVICE)
    args = ap.parse_args()
    SERVER, OUR_ID, PEER_ID, ENCODER, DEVICE = args.server, args.our_id, args.peer, args.encoder, args.device

    Gst.init(None)
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    from gi.repository import GLib  # noqa: E402
    glib_ctx = GLib.MainContext.default()

    async def pump_glib():
        while True:
            while glib_ctx.pending():
                glib_ctx.iteration(False)
            await asyncio.sleep(0.01)

    sender = Sender(loop)
    loop.create_task(pump_glib())
    loop.run_until_complete(sender.run())


if __name__ == "__main__":
    main()
