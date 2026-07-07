#!/usr/bin/env python3
"""
Foxglove WebSocket stress server — instrumented fake server for E2E-testing
PJ4's lazy-subscription feature (data_stream_foxglove_bridge).

Publishes a wide, deliberately heterogeneous mix of channels so a test can
subscribe/unsubscribe individual topics and observe that:
  - unsubscribed channels cost ~0 CPU/bandwidth (nothing is even encoded)
  - subscribing to a huge channel (e.g. /stress/cloud_huge, ~25 MB @ 2 Hz)
    doesn't stall delivery of small/high-rate channels (e.g. the 10 Hz scalars)

Channels advertised (all CDR / ros2msg, modelled on foxglove_image_server.py):
  /stress/scalar_00 .. scalar_{N-1}   std_msgs/msg/Float64        10 Hz, sine w/ per-topic phase
  /stress/image                       sensor_msgs/msg/CompressedImage  10 Hz, ~50 KB JPEG
  /stress/cloud_small                 sensor_msgs/msg/PointCloud2  2 Hz, ~64 KB
  /stress/cloud_large                 sensor_msgs/msg/PointCloud2  2 Hz, ~5 MB
  /stress/cloud_huge                  sensor_msgs/msg/PointCloud2  2 Hz, ~25 MB
  /stress/tf                          tf2_msgs/msg/TFMessage       5 Hz, 2 transforms

Only channels with at least one active subscription are ever encoded — this
mirrors the real "lazy subscription" contract under test: a client that never
subscribes to /stress/cloud_huge must never pay for it.

Usage:
    pip install foxglove-websocket pillow numpy
    python3 foxglove_stress_server.py [--host HOST] [--port PORT] [--scalars N]
"""

import argparse
import asyncio
import io
import json
import math
import struct
import time

import numpy as np
import websockets
from PIL import Image, ImageDraw

HOST = "localhost"
PORT = 8765

MESSAGE_DATA_OPCODE = 0x01

STATS_INTERVAL_S = 5.0

# High-resolution machine-readable event log for the E2E assertion checker.
# One JSON object per line: {"t": <float epoch s>, "event": ..., "topic": ...,
# "sub_id": ...}. The stdout SUBSCRIBE/UNSUBSCRIBE prints are second-granular —
# too coarse for the <1s timing invariants — so this is the source of truth.
_EVENT_LOG_FH = None


def log_event(event, topic=None, sub_id=None):
    if _EVENT_LOG_FH is None:
        return
    rec = {"t": time.time(), "event": event}
    if topic is not None:
        rec["topic"] = topic
    if sub_id is not None:
        rec["sub_id"] = sub_id
    _EVENT_LOG_FH.write(json.dumps(rec) + "\n")
    _EVENT_LOG_FH.flush()


# ---------------------------------------------------------------------------
# CDR encoding — a small generic writer.
#
# CDR alignment is relative to the *origin* of the encapsulated data, which
# begins right after the 4-byte encapsulation header (verified against
# rosx_introspection's NanoCDR decoder: `origin_ = buffer.data() + 4`). So the
# first byte written after the header is position 0, and every scalar aligns
# to its own size (1/4/8) relative to that position — NOT relative to the
# absolute start of the wire buffer (which is 4 bytes further along).
# ---------------------------------------------------------------------------


class CdrWriter:
    """Minimal little-endian CDR (v1) writer with correct alignment tracking."""

    def __init__(self):
        # Encapsulation header: [representation_id:2][options:2], little-endian.
        self._buf = bytearray(b"\x00\x01\x00\x00")

    def _pos(self) -> int:
        """Offset relative to the CDR origin (right after the 4-byte header)."""
        return len(self._buf) - 4

    def _align(self, size: int) -> None:
        pad = (-self._pos()) % size
        if pad:
            self._buf += b"\x00" * pad

    def u8(self, value: int) -> None:
        self._buf += struct.pack("<B", value & 0xFF)

    def boolean(self, value: bool) -> None:
        self.u8(1 if value else 0)

    def u32(self, value: int) -> None:
        self._align(4)
        self._buf += struct.pack("<I", value)

    def i32(self, value: int) -> None:
        self._align(4)
        self._buf += struct.pack("<i", value)

    def f32(self, value: float) -> None:
        self._align(4)
        self._buf += struct.pack("<f", value)

    def f64(self, value: float) -> None:
        self._align(8)
        self._buf += struct.pack("<d", value)

    def string(self, s: str) -> None:
        """CDR string: uint32 length (incl. NUL) + bytes + NUL terminator."""
        raw = s.encode("utf-8") + b"\x00"
        self.u32(len(raw))
        self._buf += raw

    def u8_sequence(self, data: bytes) -> None:
        """sequence<uint8>: uint32 count + raw bytes (uint8 needs no alignment)."""
        self.u32(len(data))
        self._buf += data

    def f32_sequence_raw(self, arr: np.ndarray) -> None:
        """Append a pre-packed little-endian float32 array as raw payload bytes,
        after aligning to 4 (the caller is responsible for having emitted the
        matching uint32 element count beforehand, e.g. via u32())."""
        self._align(4)
        self._buf += arr.astype("<f4", copy=False).tobytes()

    def bytes(self) -> bytes:
        return bytes(self._buf)


def stamp_from_t(t: float) -> tuple:
    ns = int(t * 1e9)
    return ns // 1_000_000_000, ns % 1_000_000_000


def write_header(w: CdrWriter, t: float, frame_id: str) -> None:
    """std_msgs/msg/Header: builtin_interfaces/Time stamp + string frame_id."""
    sec, nanosec = stamp_from_t(t)
    w.i32(sec)
    w.u32(nanosec)
    w.string(frame_id)


# ---------------------------------------------------------------------------
# ROS2 message schemas (.msg format) — flattened with all nested types, in
# the same "80x'=' separator + MSG: <type>" convention as foxglove_image_server.py.
# Note: schemaName carries the /msg/ segment; the schema *body* type
# references (field types and MSG: headers) do not, matching upstream rosmsg
# text and the existing image_server precedent.
# ---------------------------------------------------------------------------

SEP = "=" * 80

HEADER_DEPS = f"""\
{SEP}
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
{SEP}
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
"""

FLOAT64_SCHEMA = "float64 data\n"

COMPRESSED_IMAGE_SCHEMA = f"""\
std_msgs/Header header
string format
uint8[] data
{HEADER_DEPS}"""

POINTCLOUD2_SCHEMA = f"""\
std_msgs/Header header
uint32 height
uint32 width
sensor_msgs/PointField[] fields
bool is_bigendian
uint32 point_step
uint32 row_step
uint8[] data
bool is_dense
{HEADER_DEPS}\
{SEP}
MSG: sensor_msgs/PointField
uint8 INT8    = 1
uint8 UINT8   = 2
uint8 INT16   = 3
uint8 UINT16  = 4
uint8 INT32   = 5
uint8 UINT32  = 6
uint8 FLOAT32 = 7
uint8 FLOAT64 = 8

string name
uint32 offset
uint8 datatype
uint32 count
"""

TFMESSAGE_SCHEMA = f"""\
geometry_msgs/TransformStamped[] transforms
{SEP}
MSG: geometry_msgs/TransformStamped
std_msgs/Header header
string child_frame_id
geometry_msgs/Transform transform
{HEADER_DEPS}\
{SEP}
MSG: geometry_msgs/Transform
geometry_msgs/Vector3 translation
geometry_msgs/Quaternion rotation
{SEP}
MSG: geometry_msgs/Vector3
float64 x
float64 y
float64 z
{SEP}
MSG: geometry_msgs/Quaternion
float64 x
float64 y
float64 z
float64 w
"""

POINTFIELD_FLOAT32 = 7  # sensor_msgs/msg/PointField.FLOAT32


# ---------------------------------------------------------------------------
# Per-message encoders
# ---------------------------------------------------------------------------


def encode_float64_cdr(value: float) -> bytes:
    w = CdrWriter()
    w.f64(value)
    return w.bytes()


WIDTH, HEIGHT = 320, 240


def make_frame(t: float) -> bytes:
    """~50 KB JPEG: animated gradient + per-frame noise (keeps JPEG entropy up
    so the compressed size stays in a narrow, predictable band) + bouncing ball."""
    xs = np.arange(WIDTH)
    ys = np.arange(HEIGHT)
    r_shift = int(127 + 127 * math.sin(t * 0.5))
    g_shift = int(127 + 127 * math.sin(t * 0.7 + 2.0))
    b_shift = int(127 + 127 * math.sin(t * 0.3 + 4.0))

    img = np.zeros((HEIGHT, WIDTH, 3), dtype=np.int16)
    img[:, :, 0] = (r_shift + xs[None, :]) % 256
    img[:, :, 1] = (g_shift + ys[:, None]) % 256
    img[:, :, 2] = b_shift

    rng = np.random.default_rng(int(t * 1000) & 0xFFFFFFFF)
    noise = rng.integers(-40, 40, size=(HEIGHT, WIDTH, 3))
    img = np.clip(img + noise, 0, 255).astype(np.uint8)

    pil_img = Image.fromarray(img, "RGB")
    draw = ImageDraw.Draw(pil_img)
    bx = int(WIDTH * 0.5 + (WIDTH * 0.4) * math.sin(t * 1.2))
    by = int(HEIGHT * 0.5 + (HEIGHT * 0.4) * math.cos(t * 0.9))
    draw.ellipse([bx - 15, by - 15, bx + 15, by + 15], fill=(255, 255, 255))
    draw.rectangle([0, 0, 160, 20], fill=(0, 0, 0))
    draw.text((4, 4), f"t={t:.2f}s", fill=(255, 255, 0))

    buf = io.BytesIO()
    pil_img.save(buf, format="JPEG", quality=93)
    return buf.getvalue()


def encode_compressed_image_cdr(t: float) -> bytes:
    """CDR-encode sensor_msgs/msg/CompressedImage."""
    jpeg_bytes = make_frame(t)
    w = CdrWriter()
    write_header(w, t, "camera")
    w.string("jpeg")
    w.u8_sequence(jpeg_bytes)
    return w.bytes()


class PointCloudSource:
    """Precomputes a base point cloud once (cheap RNG draw), then cheaply
    animates it per-frame with a rotation — avoids re-drawing millions of
    random numbers on every tick for the huge cloud."""

    def __init__(self, n_points: int, radius: float, seed: int):
        rng = np.random.default_rng(seed)
        vecs = rng.normal(size=(n_points, 3)).astype(np.float64)
        vecs /= np.linalg.norm(vecs, axis=1, keepdims=True)
        shell = np.cbrt(rng.uniform(0.15, 1.0, size=(n_points, 1)))
        self.base = (vecs * shell * radius)
        self.n_points = n_points

    def points_at(self, t: float) -> np.ndarray:
        c, s = math.cos(t * 0.3), math.sin(t * 0.3)
        rot = np.array([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])
        return self.base @ rot.T


def encode_pointcloud2_cdr(t: float, frame_id: str, points: np.ndarray) -> bytes:
    """CDR-encode sensor_msgs/msg/PointCloud2 with packed float32 x,y,z."""
    n_points = points.shape[0]
    point_step = 12  # 3 x float32
    row_step = point_step * n_points
    data = points.astype("<f4", copy=False).tobytes()

    w = CdrWriter()
    write_header(w, t, frame_id)
    w.u32(1)  # height
    w.u32(n_points)  # width

    fields = [("x", 0), ("y", 4), ("z", 8)]
    w.u32(len(fields))  # fields sequence count
    for name, offset in fields:
        w.string(name)
        w.u32(offset)
        w.u8(POINTFIELD_FLOAT32)
        w.u32(1)  # count

    w.boolean(False)  # is_bigendian
    w.u32(point_step)
    w.u32(row_step)
    w.u8_sequence(data)
    w.boolean(True)  # is_dense
    return w.bytes()


def encode_tf_message_cdr(t: float, transforms: list) -> bytes:
    """CDR-encode tf2_msgs/msg/TFMessage: sequence<TransformStamped>."""
    w = CdrWriter()
    w.u32(len(transforms))
    for frame_id, child_frame_id, translation, rotation in transforms:
        write_header(w, t, frame_id)
        w.string(child_frame_id)
        for v in translation:
            w.f64(v)
        for v in rotation:
            w.f64(v)
    return w.bytes()


def build_binary_frame(subscription_id: int, log_time_ns: int, cdr: bytes) -> bytes:
    """[opcode:1][subscription_id:4 LE][log_time_ns:8 LE][cdr_payload]"""
    return struct.pack("<BIQ", MESSAGE_DATA_OPCODE, subscription_id, log_time_ns) + cdr


# ---------------------------------------------------------------------------
# Channel catalog
# ---------------------------------------------------------------------------


def build_channels(num_scalars: int):
    channels = []
    width = max(2, len(str(num_scalars - 1)))
    scalar_ids = []
    next_id = 1

    for i in range(num_scalars):
        cid = next_id
        next_id += 1
        scalar_ids.append(cid)
        channels.append({
            "id": cid,
            "topic": f"/stress/scalar_{i:0{width}d}",
            "encoding": "cdr",
            "schemaName": "std_msgs/msg/Float64",
            "schema": FLOAT64_SCHEMA,
            "schemaEncoding": "ros2msg",
        })

    image_id = next_id
    next_id += 1
    channels.append({
        "id": image_id,
        "topic": "/stress/image",
        "encoding": "cdr",
        "schemaName": "sensor_msgs/msg/CompressedImage",
        "schema": COMPRESSED_IMAGE_SCHEMA,
        "schemaEncoding": "ros2msg",
    })

    cloud_specs = [
        ("cloud_small", 64 * 1024),
        ("cloud_large", 5 * 1024 * 1024),
        ("cloud_huge", 25 * 1024 * 1024),
    ]
    cloud_ids = {}
    cloud_sources = {}
    for name, target_bytes in cloud_specs:
        cid = next_id
        next_id += 1
        n_points = max(1, target_bytes // 12)
        cloud_ids[name] = cid
        cloud_sources[cid] = PointCloudSource(n_points, radius=2.0, seed=hash(name) & 0xFFFF)
        channels.append({
            "id": cid,
            "topic": f"/stress/{name}",
            "encoding": "cdr",
            "schemaName": "sensor_msgs/msg/PointCloud2",
            "schema": POINTCLOUD2_SCHEMA,
            "schemaEncoding": "ros2msg",
        })

    tf_id = next_id
    next_id += 1
    channels.append({
        "id": tf_id,
        "topic": "/stress/tf",
        "encoding": "cdr",
        "schemaName": "tf2_msgs/msg/TFMessage",
        "schema": TFMESSAGE_SCHEMA,
        "schemaEncoding": "ros2msg",
    })

    return channels, scalar_ids, image_id, cloud_ids, cloud_sources, tf_id


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------


class FoxgloveStressServer:
    def __init__(self, num_scalars: int):
        (
            self.channels,
            self.scalar_ids,
            self.image_id,
            self.cloud_ids,
            self.cloud_sources,
            self.tf_id,
        ) = build_channels(num_scalars)
        self.channel_by_id = {ch["id"]: ch for ch in self.channels}

        self.clients: dict = {}
        self._readvertise_tasks: dict = {}

        # Instrumentation: channel_id -> {"messages": int, "bytes": int}
        self.stats = {ch["id"]: {"messages": 0, "bytes": 0} for ch in self.channels}
        self._last_stats = {cid: dict(v) for cid, v in self.stats.items()}
        self._last_stats_time = time.monotonic()

    # -- connection lifecycle ------------------------------------------------

    async def handler(self, websocket):
        client_id = id(websocket)
        self.clients[websocket] = {}
        print(f"[+] Client connected: {client_id}")

        log_event("connect")
        await websocket.send(json.dumps({"op": "advertise", "channels": self.channels}))
        log_event("advertise")
        print(f"    advertise → {len(self.channels)} channels "
              f"({len(self.scalar_ids)} scalars, image, "
              f"{len(self.cloud_ids)} clouds, tf)")

        task = asyncio.create_task(self._readvertise_until_subscribed(websocket))
        self._readvertise_tasks[websocket] = task

        try:
            async for message in websocket:
                await self.on_message(websocket, message)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            task.cancel()
            self._readvertise_tasks.pop(websocket, None)
            del self.clients[websocket]
            print(f"[-] Client disconnected: {client_id}")

    async def _readvertise_until_subscribed(self, websocket):
        try:
            while True:
                await asyncio.sleep(2.0)
                if not self.clients.get(websocket):
                    await websocket.send(json.dumps({"op": "advertise", "channels": self.channels}))
                    print("    re-advertise → waiting for subscribe")
                else:
                    break
        except (asyncio.CancelledError, websockets.exceptions.ConnectionClosed):
            pass

    async def on_message(self, websocket, message):
        if isinstance(message, bytes):
            return
        try:
            msg = json.loads(message)
        except json.JSONDecodeError:
            return

        op = msg.get("op", "")
        if op == "subscribe":
            for sub in msg.get("subscriptions", []):
                sub_id = sub.get("id")
                channel_id = sub.get("channelId")
                if sub_id is not None and channel_id in self.channel_by_id:
                    self.clients[websocket][channel_id] = sub_id
                    log_event("subscribe", self.channel_by_id[channel_id]["topic"], sub_id)
            topics = [self.channel_by_id[c]["topic"] for c in self.clients[websocket]]
            print(f"\n    SUBSCRIBE  → {topics} at wallclock {time.strftime('%H:%M:%S')}")
        elif op == "unsubscribe":
            sub_ids = msg.get("subscriptionIds", [])
            unsub_topics = [
                self.channel_by_id[ch_id]["topic"]
                for ch_id, s_id in self.clients[websocket].items()
                if s_id in sub_ids
            ]
            for ch_id, s_id in self.clients[websocket].items():
                if s_id in sub_ids:
                    log_event("unsubscribe", self.channel_by_id[ch_id]["topic"], s_id)
            print(f"\n    UNSUBSCRIBE → {unsub_topics} at wallclock {time.strftime('%H:%M:%S')}")
            self.clients[websocket] = {
                ch_id: s_id
                for ch_id, s_id in self.clients[websocket].items()
                if s_id not in sub_ids
            }

    # -- subscription helpers -------------------------------------------------

    def _subscribers(self, channel_id: int):
        """Yield (websocket, subscription_id) for every client subscribed to channel_id."""
        for websocket, subs in list(self.clients.items()):
            sub_id = subs.get(channel_id)
            if sub_id is not None:
                yield websocket, sub_id

    def _has_subscribers(self, channel_id: int) -> bool:
        return any(channel_id in subs for subs in self.clients.values())

    async def _publish(self, channel_id: int, cdr: bytes) -> int:
        """Send `cdr` to every current subscriber of channel_id. Returns
        subscriber count (0 means the caller shouldn't have encoded at all —
        callers are expected to check _has_subscribers() first)."""
        log_time_ns = time.time_ns()
        count = 0
        for websocket, sub_id in self._subscribers(channel_id):
            frame = build_binary_frame(sub_id, log_time_ns, cdr)
            try:
                await websocket.send(frame)
                count += 1
            except websockets.exceptions.ConnectionClosed:
                pass
        if count:
            self.stats[channel_id]["messages"] += 1
            self.stats[channel_id]["bytes"] += len(cdr) * count
        return count

    # -- emit loops ------------------------------------------------------------

    async def emit_scalars(self):
        """10 Hz sine wave per scalar topic, per-topic phase offset."""
        interval = 0.1
        t = 0.0
        n = len(self.scalar_ids)
        while True:
            await asyncio.sleep(interval)
            t += interval
            for idx, channel_id in enumerate(self.scalar_ids):
                if not self._has_subscribers(channel_id):
                    continue
                phase = (2 * math.pi * idx) / max(n, 1)
                value = math.sin(2 * math.pi * 0.5 * t + phase)
                cdr = encode_float64_cdr(value)
                await self._publish(channel_id, cdr)

    async def emit_image(self):
        """10 Hz ~50 KB JPEG frame."""
        interval = 0.1
        t = 0.0
        while True:
            await asyncio.sleep(interval)
            t += interval
            if not self._has_subscribers(self.image_id):
                continue
            cdr = encode_compressed_image_cdr(t)
            await self._publish(self.image_id, cdr)

    async def emit_clouds(self):
        """2 Hz PointCloud2 for small/large/huge — only encodes clouds that
        currently have at least one subscriber (this is the crux of the
        lazy-subscription contract: cloud_huge must be free when idle)."""
        interval = 0.5
        t = 0.0
        while True:
            await asyncio.sleep(interval)
            t += interval
            for name, channel_id in self.cloud_ids.items():
                if not self._has_subscribers(channel_id):
                    continue
                source = self.cloud_sources[channel_id]
                points = source.points_at(t)
                cdr = encode_pointcloud2_cdr(t, "map", points)
                await self._publish(channel_id, cdr)

    async def emit_tf(self):
        """5 Hz TFMessage with 2 animated transforms."""
        interval = 0.2
        t = 0.0
        while True:
            await asyncio.sleep(interval)
            t += interval
            if not self._has_subscribers(self.tf_id):
                continue
            transforms = [
                (
                    "world", "robot1",
                    (2.0 * math.cos(t * 0.5), 2.0 * math.sin(t * 0.5), 0.0),
                    (0.0, 0.0, math.sin(t * 0.25), math.cos(t * 0.25)),
                ),
                (
                    "world", "robot2",
                    (-1.5 * math.sin(t * 0.3), 1.5 * math.cos(t * 0.3), 0.5),
                    (0.0, 0.0, math.sin(-t * 0.15), math.cos(-t * 0.15)),
                ),
            ]
            cdr = encode_tf_message_cdr(t, transforms)
            await self._publish(self.tf_id, cdr)

    async def report_stats(self):
        """Periodic instrumentation: per-channel subscriber count + throughput
        since the last report, so an E2E test (or a human) can eyeball that
        unsubscribed channels are truly at zero cost."""
        while True:
            await asyncio.sleep(STATS_INTERVAL_S)
            now = time.monotonic()
            elapsed = now - self._last_stats_time
            self._last_stats_time = now

            lines = []
            for ch in self.channels:
                cid = ch["id"]
                cur = self.stats[cid]
                prev = self._last_stats[cid]
                d_msgs = cur["messages"] - prev["messages"]
                d_bytes = cur["bytes"] - prev["bytes"]
                self._last_stats[cid] = dict(cur)
                n_subs = sum(1 for subs in self.clients.values() if cid in subs)
                if n_subs == 0 and d_msgs == 0:
                    continue
                rate = d_msgs / elapsed if elapsed > 0 else 0.0
                bw = d_bytes / elapsed if elapsed > 0 else 0.0
                lines.append(
                    f"      {ch['topic']:<24s} subs={n_subs}  "
                    f"{rate:6.2f} msg/s  {bw / 1024:9.1f} KB/s  "
                    f"(total {cur['messages']} msgs, {cur['bytes'] / 1e6:.2f} MB)"
                )

            if lines:
                print(f"\n  [stats @ {time.strftime('%H:%M:%S')}] active channels:")
                print("\n".join(lines))
            else:
                print(f"\n  [stats @ {time.strftime('%H:%M:%S')}] idle (no subscriptions)")


async def main(host: str, port: int, num_scalars: int):
    server = FoxgloveStressServer(num_scalars)
    print(f"Foxglove stress server listening on ws://{host}:{port}")
    print(f"Channels: {len(server.channels)} total")
    print(f"  /stress/scalar_00..{num_scalars - 1:02d}  std_msgs/msg/Float64        10 Hz")
    print(f"  /stress/image                sensor_msgs/msg/CompressedImage  10 Hz  ~50 KB")
    print(f"  /stress/cloud_small          sensor_msgs/msg/PointCloud2  2 Hz  ~64 KB")
    print(f"  /stress/cloud_large          sensor_msgs/msg/PointCloud2  2 Hz  ~5 MB")
    print(f"  /stress/cloud_huge           sensor_msgs/msg/PointCloud2  2 Hz  ~25 MB")
    print(f"  /stress/tf                   tf2_msgs/msg/TFMessage       5 Hz")
    print("Nothing is encoded/sent for a channel until a client subscribes to it.")
    print("Ctrl+C to stop\n")

    async with websockets.serve(
        server.handler,
        host,
        port,
        subprotocols=["foxglove.sdk.v1"],
        max_size=None,  # cloud_huge frames (~25 MB) exceed the websockets default limit
    ):
        await asyncio.gather(
            server.emit_scalars(),
            server.emit_image(),
            server.emit_clouds(),
            server.emit_tf(),
            server.report_stats(),
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Foxglove stress server — lazy-subscription E2E fixture")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--scalars", type=int, default=20, help="number of scalar topics (default: 20)")
    parser.add_argument("--event-log", default=None,
                        help="write high-resolution JSONL subscribe/unsubscribe events here (E2E checker source of truth)")
    args = parser.parse_args()

    if args.event_log:
        _EVENT_LOG_FH = open(args.event_log, "w")
        print(f"Event log → {args.event_log}")

    try:
        asyncio.run(main(args.host, args.port, args.scalars))
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if _EVENT_LOG_FH is not None:
            _EVENT_LOG_FH.close()
