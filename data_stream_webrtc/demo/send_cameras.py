#!/usr/bin/env python3
"""Multi-camera GStreamer webrtcbin sender (OFFERER) for the PJ4 WebRTC demo.

This is the multi-track sibling of send_camera.py. It builds ONE webrtcbin that
carries one H.264 sendonly track per requested camera, then rewrites the offer
SDP so that each video m-line's `a=mid:<x>` equals the camera's stream id (the
`mid == stream-id` contract, see PROTOCOL / demo/README.md). The plugin
(libdatachannel) is always the ANSWERER; this peer always OFFERS.

Cameras are passed positionally, one per requested stream, as:

    <id>=<source>:<device>

where <source> is `v4l2` (real camera; <device> is e.g. /dev/video0) or
`videotestsrc` (synthetic; <device> ignored). Examples:

    python3 send_cameras.py --server ws://127.0.0.1:8443 --peer receiver \
        cam0=v4l2:/dev/video0 cam1=videotestsrc:

Pad order is the spec order, which is the SDP m-line order, which is what the
mid-rewrite keys on — so keep the positional order stable.

Usually launched for you by stream_server.py (which derives the spec list from
the receiver's `subscribe`); runnable standalone too. The single-camera legacy
sender send_camera.py is still the right tool for non-advertising / manual mode.

Deps: python3-gi + GStreamer GI (gst-plugins-bad webrtcbin, nice, good, base)
      + websockets. See demo/README.md for the apt package list.

GOTCHA: `gi` MUST be imported (and versions required) BEFORE `websockets`.
Importing websockets first pulls in an event-loop/SSL stack that segfaults some
PyGObject builds when Gst is initialized afterwards. Keep this import order.
"""
import argparse
import asyncio
import json
import re
import sys

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import Gst, GstWebRTC, GstSdp  # noqa: E402

import websockets  # noqa: E402


def encoder_chain(enc):
    """H.264 encode chain shared with send_camera.py.

    config-interval=-1 (set on the payloader tail) re-sends SPS/PPS with every
    IDR so a mid-stream join can decode; the encoder just produces frequent IDRs.
    """
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


class Spec:
    """One requested camera: stream id + how to source its pixels."""

    __slots__ = ("id", "source", "device")

    def __init__(self, sid, source, device):
        self.id = sid
        self.source = source
        self.device = device

    @classmethod
    def parse(cls, token):
        # <id>=<source>:<device>  (device may be empty for videotestsrc)
        if "=" not in token:
            raise SystemExit("bad camera spec {!r}; want <id>=<source>:<device>".format(token))
        sid, rest = token.split("=", 1)
        if ":" in rest:
            source, device = rest.split(":", 1)
        else:
            source, device = rest, ""
        sid = sid.strip()
        source = source.strip() or "videotestsrc"
        if not sid:
            raise SystemExit("camera spec {!r} has an empty id".format(token))
        return cls(sid, source, device.strip())


def source_chain(spec, index):
    """Pixel source for one camera. Emits raw frames; the encoder_chain owns the
    colour conversion (mirrors send_camera.py: `v4l2src ! <encoder_chain>`).

    v4l2:         a real camera at spec.device.
    videotestsrc: a synthetic live pattern; `pattern=<index>` makes each
                  synthetic camera visually distinct, and is-live=true paces it.
    """
    if spec.source == "v4l2":
        dev = spec.device or "/dev/video0"
        return "v4l2src device={dev}".format(dev=dev)
    if spec.source == "videotestsrc":
        return ("videotestsrc is-live=true pattern={pat} ! "
                "video/x-raw,width=640,height=480,framerate=30/1".format(pat=index % 25))
    raise SystemExit("unknown camera source {!r} (want v4l2 | videotestsrc)".format(spec.source))


def build_pipeline(specs, enc):
    """ONE webrtcbin (bundle-policy=max-bundle) with one sendonly H.264 branch
    per spec. Each branch ends at sendrecv.sink_<idx> in spec order, so the i-th
    m-line corresponds to specs[i] — the invariant the mid-rewrite relies on."""
    branches = []
    for idx, spec in enumerate(specs):
        branches.append(
            "{src} ! {chain} ! "
            "rtph264pay config-interval=-1 pt=96 ! "
            "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
            "sendrecv.".format(src=source_chain(spec, idx), chain=encoder_chain(enc)))
    desc = "webrtcbin name=sendrecv bundle-policy=max-bundle " + " ".join(branches)
    print("PIPELINE:", desc, file=sys.stderr)
    return Gst.parse_launch(desc)


# WHY string-rewrite the SDP at all: webrtcbin assigns its OWN transceiver mids
# (video0, video1, ...) and the installed GStreamer exposes no API to rename them,
# so we can't make it emit the stream-id mids the plugin keys tracks on. Instead we
# rewrite only the OFFER we put on the wire (rewrite_mids) and map the inbound ANSWER
# back to webrtcbin's mids (restore_mids) so set-remote-description still matches.
def rewrite_mids(sdp_text, stream_ids):
    """Rename each video m-line's `a=mid:videoN` to `a=mid:<stream-id>` in
    m-line order, replace webrtcbin's `a=msid:` with `a=msid:<id> <id>`, and
    rebuild `a=group:BUNDLE` to match the new mids. CRLF-safe.

    The mid rename keys on m-line order only, which is safe here because every
    m-line we emit is a video m-line we control (one per spec). If a future
    pipeline adds non-video m-lines, gate the rename on an `m=video` section.
    """
    out = []
    k = 0
    bundle = []
    for line in sdp_text.splitlines():
        if line.startswith("a=mid:") and k < len(stream_ids):
            sid = stream_ids[k]
            k += 1
            out.append("a=mid:" + sid)
            out.append("a=msid:" + sid + " " + sid)
            bundle.append(sid)
        elif line.startswith("a=msid:"):
            # Drop webrtcbin's own msid; we inject our own right after a=mid.
            continue
        else:
            out.append(line)
    text = "\r\n".join(out) + "\r\n"
    if bundle:
        text = re.sub(r"a=group:BUNDLE[^\r\n]*", "a=group:BUNDLE " + " ".join(bundle), text, count=1)
    return text


def restore_mids(sdp_text, original_mids):
    """Inverse of the wire-offer rewrite: rename each m-line's `a=mid` (in m-line
    order) back to webrtcbin's OWN mids and rebuild `a=group:BUNDLE`. The plugin
    answers with the wire (stream-id) mids; webrtcbin's transceivers still carry
    its internal mids, so set-remote-description aborts on a mid/mline mismatch
    unless the answer is mapped back first. CRLF-safe."""
    out = []
    k = 0
    for line in sdp_text.splitlines():
        if line.startswith("a=mid:") and k < len(original_mids):
            out.append("a=mid:" + original_mids[k])
            k += 1
        else:
            out.append(line)
    text = "\r\n".join(out) + "\r\n"
    if k:
        text = re.sub(r"a=group:BUNDLE[^\r\n]*", "a=group:BUNDLE " + " ".join(original_mids[:k]), text, count=1)
    return text


class Sender:
    def __init__(self, loop, specs, enc):
        self.loop = loop
        self.specs = specs
        self.enc = enc
        self.stream_ids = [s.id for s in specs]  # sink-pad / m-line order
        self.original_mids = []  # webrtcbin's own mids, captured from the offer
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
        # set-local-description with the ORIGINAL offer (webrtcbin's internal
        # mids); only the SDP we PUT ON THE WIRE carries the rewritten mids.
        offer_text = offer.sdp.as_text()
        self.original_mids = re.findall(r"^a=mid:(\S+)", offer_text, re.M)
        p2 = Gst.Promise.new()
        element.emit("set-local-description", offer, p2)
        p2.interrupt()
        wire_sdp = rewrite_mids(offer_text, self.stream_ids)
        self.send(json.dumps({"sdp": {"type": "offer", "sdp": wire_sdp}}))

    def on_ice_candidate(self, _element, mlineindex, candidate):
        self.send(json.dumps({"ice": {"candidate": candidate, "sdpMLineIndex": mlineindex}}))

    def start_pipeline(self):
        self.pipe = build_pipeline(self.specs, self.enc)
        self.webrtc = self.pipe.get_by_name("sendrecv")
        self.webrtc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self.on_ice_candidate)
        self.pipe.set_state(Gst.State.PLAYING)

    def handle_sdp(self, sdp_obj):
        assert sdp_obj["type"] == "answer", "sender is offerer; expected an answer"
        # The plugin answered with the wire (stream-id) mids; map them back to
        # webrtcbin's own mids so set-remote-description matches its transceivers.
        answer_text = restore_mids(sdp_obj["sdp"], self.original_mids)
        _res, sdpmsg = GstSdp.SDPMessage.new_from_text(answer_text)
        answer = GstWebRTC.WebRTCSessionDescription.new(GstWebRTC.WebRTCSDPType.ANSWER, sdpmsg)
        promise = Gst.Promise.new()
        self.webrtc.emit("set-remote-description", answer, promise)
        promise.interrupt()

    def handle_ice(self, ice):
        self.webrtc.emit("add-ice-candidate", ice["sdpMLineIndex"], ice["candidate"])

    async def run(self, server, our_id, peer_id):
        self.conn = await websockets.connect(server)
        await self.conn.send("HELLO " + our_id)
        assert await self.conn.recv() == "HELLO"
        await self.conn.send("SESSION " + peer_id)
        assert await self.conn.recv() == "SESSION_OK"
        self.start_pipeline()  # triggers on-negotiation-needed -> ONE multi-m-line offer
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
    ap = argparse.ArgumentParser(description="PJ4 WebRTC multi-camera sender (offerer).")
    ap.add_argument("--server", default="ws://127.0.0.1:8443", help="signaling ws url")
    ap.add_argument("--our-id", dest="our_id", default="sender")
    ap.add_argument("--peer", default="receiver", help="peer id to SESSION to (the plugin's 'Our peer id')")
    ap.add_argument("--encoder", default="x264",
                    choices=["x264", "vaapi", "nvenc", "jetson", "passthrough"])
    ap.add_argument("cameras", nargs="+", metavar="ID=SOURCE:DEVICE",
                    help="one per stream, e.g. cam0=v4l2:/dev/video0 cam1=videotestsrc:")
    args = ap.parse_args()

    specs = [Spec.parse(tok) for tok in args.cameras]
    seen = set()
    for s in specs:
        if s.id in seen:
            raise SystemExit("duplicate camera id {!r}".format(s.id))
        seen.add(s.id)

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

    sender = Sender(loop, specs, args.encoder)
    loop.create_task(pump_glib())
    loop.run_until_complete(sender.run(args.server, args.our_id, args.peer))


if __name__ == "__main__":
    main()
