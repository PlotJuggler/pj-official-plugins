<!--
Copyright 2026 Davide Faconti
SPDX-License-Identifier: MIT
-->

# data_stream_webrtc Signaling & Negotiation Protocol

**Protocol version: `1`**

This document is the contract that the `data_stream_webrtc` PlotJuggler plugin
(the **receiver**) and any compatible video sender (the **streamer**) both
implement. It defines:

- the WebSocket signaling messages (HELLO/SESSION handshake, catalog discovery,
  subscribe, SDP/ICE relay),
- the **`mid == stream-id`** contract that lets one PeerConnection carry many
  cameras,
- the answerer/offerer roles and the recvonly multi-track negotiation,
- the `protocol` version field and how mismatches are handled,
- the manual / legacy single-stream fallback for senders that do not advertise,
- the exact C++-facing data structures and signaling API additions.

It is a strict, backward-compatible **superset** of the GStreamer
`webrtc_sendrecv` "simple signaling" protocol that the plugin shipped with
originally. A sender that implements only the original handshake (HELLO/SESSION
+ verbatim `{"sdp"}`/`{"ice"}` relay) keeps working unchanged via the
[manual / legacy fallback](#7-manual--legacy-single-stream-fallback).

---

## 1. Roles and transport

| Role | Who | WebRTC role | Signaling behavior |
|------|-----|-------------|--------------------|
| **Receiver** | the PlotJuggler plugin (libdatachannel) | **answerer**, all tracks `recvonly` | registers, discovers, subscribes, answers offers |
| **Streamer** | the robot / camera process (e.g. GStreamer `webrtcbin`, or `demo/send_cameras.py`) | **offerer** | advertises a catalog, builds one offer with one m-line per requested stream |
| **Broker** | the signaling server (`demo/stream_server.py`) | none (relay + catalog/subscribe bookkeeping) | HELLO/SESSION + verbatim SDP/ICE relay + owns the catalog and supervises the streamer |

Key invariants (unchanged from the legacy protocol):

- **Signaling is out-of-band** over a single `rtc::WebSocket` (text frames). The
  broker never inspects SDP/ICE; once two peers are in a session it relays every
  SDP/ICE message between them verbatim.
- **The receiver is always the answerer.** It never emits an SDP offer.
  libdatachannel applies the remote offer, mints the recvonly tracks from it, and
  produces the answer.
- **One PeerConnection per session.** All selected cameras ride a single
  `rtc::PeerConnection` as N `recvonly` H.264 m-lines, BUNDLE'd
  (`bundle-policy=max-bundle` on the offerer). There is no PeerConnection per
  camera.

---

## 2. The `mid == stream-id` contract

> **For every video m-line, the SDP media id (`a=mid:<x>`) is the stream id.**

Consequences both sides MUST honor:

1. The streamer, when answering a `subscribe` for streams `["cam0","cam2"]`,
   emits an offer with one recvonly H.264 m-line per requested stream and sets
   `a=mid:cam0`, `a=mid:cam2`. It SHOULD also set `a=msid:<stream-id> <stream-id>`
   on that m-line.
2. The receiver, on `pc_->onTrack`, reads `track->mid()` and uses it **directly**
   as the stream id. `mid` is the join key from inbound RTP track →
   `DiscoveredStream` the user selected → the PlotJuggler topic for that camera.
3. ICE candidates are relayed with the numeric `sdpMLineIndex`; the receiver
   recovers the owning mid from an ordered `mline_index → mid` table it builds
   when it applies the offer (see §6.6). With BUNDLE (`max-bundle`) candidates
   apply to the whole bundle, so the index is sufficient.

A stream id is an opaque, stable, URL/SDP-safe token. **Constraints:** non-empty,
ASCII, no whitespace, matches `^[A-Za-z0-9_.-]+$`, max 64 chars.

---

## 3. Message envelope and versioning

All signaling messages are either:

- **bare control strings** inherited from the legacy protocol — `HELLO <id>`,
  `HELLO`, `SESSION <id>`, `SESSION_OK`, `ERROR <reason>` — or
- **JSON objects** with a `"type"` field. Legacy `{"sdp":…}` / `{"ice":…}`
  objects (which have no `"type"`) are still accepted unchanged (see §6).

Every JSON control message introduced by **this** version carries a `protocol`
integer:

```json
{ "type": "catalog", "protocol": 1, "streams": [ /* … */ ] }
```

### 3.1 Version negotiation rules

- The plugin advertises that it speaks `protocol >= 1` implicitly by sending a
  `list` request and/or a `subscribe` (both carry `"protocol": 1`).
- A streamer that understands `protocol 1` responds with a `catalog`
  (`"protocol": 1`).
- **Forward compatibility:** a receiver of any versioned message MUST ignore
  unknown top-level keys and unknown array-element keys.
- **Mismatch handling:**
  - If the streamer's `catalog.protocol` is **greater** than the receiver
    supports, the receiver uses only the fields it understands and proceeds; if a
    required field is absent it falls back to manual mode (§7) and warns.
  - If a `subscribe` arrives at a streamer with an unsupported `protocol`, the
    streamer SHOULD reply `ERROR unsupported protocol <n>` and MAY fall back to
    offering its single default stream.
  - A peer that sends **no** versioned messages is, by definition, a legacy
    single-stream sender; the receiver uses the manual fallback (§7).

There is exactly one major version today (`1`). Bumping it is reserved for an
incompatible change to the handshake, the mid contract, or subscribe semantics.

---

## 4. Discovery flow (advertise → select)

```
  receiver (plugin)                 broker                  streamer (robot)
        |                             |                            |
        |  ws connect + "HELLO recv"  |                            |
        |---------------------------->|   "HELLO"                  |
        |<----------------------------|                            |
        |  (broker pushes catalog on HELLO; no SESSION required    |
        |   for discovery in the demo broker)                      |
        |   {"type":"catalog","protocol":1,"streams":[ … ]}        |
        |<----------------------------|                            |
        |                             |                            |
   (dialog shows the catalog; user multi-selects cam0, cam2)       |
```

The streamer/broker MAY push an **unsolicited** `catalog` immediately after the
peer registers (the demo broker does this on `HELLO` from the receiver id), so a
`list` request is optional. The receiver treats a `catalog` arriving at any time
(before subscribe) as the current truth and re-renders the discovery table. If no
`catalog` arrives within a UI timeout, the dialog offers the manual fallback (§7).

---

## 5. Subscribe & multi-track negotiation flow

```
  receiver (plugin, ANSWERER)        broker                streamer (OFFERER)
        |                             |                            |
        |  {"type":"subscribe","protocol":1,"streams":["cam0","cam2"]}
        |---------------------------->|  (broker launches/configures the        |
        |                             |   streamer for cam0,cam2)               |
        |                             |   builds ONE offer:        |
        |                             |     m=video … a=mid:cam0  recvonly@sender
        |                             |     m=video … a=mid:cam2  recvonly@sender
        |   {"sdp":{"type":"offer","sdp":"…2 m-lines…"}}           |
        |<=========================== relayed =====================|
        |                             |                            |
   pc_.setRemoteDescription(offer)    |                            |
   -> onTrack fires twice:            |                            |
        track.mid()=="cam0"           |                            |
        track.mid()=="cam2"           |                            |
   pc_ creates ANSWER (both recvonly) |                            |
        |   {"sdp":{"type":"answer","sdp":"…"}}                    |
        |============================ relayed =====================>|
        |   {"ice":{ … sdpMLineIndex:0 … }}  (both directions, trickle)
        |<==========================================================>|
        |                             |                            |
   DTLS/SRTP up → H.264 RTP per track → 1 PJ.VideoFrame topic per mid
```

### 5.1 What "subscribe" guarantees

- The streamer offers **exactly** the requested streams, in any order, each as a
  recvonly H.264 m-line with `a=mid:<stream-id>`. Unknown ids are omitted; if
  **none** are known the streamer replies `ERROR no valid streams in subscribe`
  (see §6.7).
- Re-subscription (selection changed while connected) is handled by the streamer
  issuing a renegotiation offer with the new m-line set. Receivers MUST accept a
  second offer on the same PeerConnection and re-answer. (Simplest conforming
  streamer: tear down and re-offer; the demo restarts the pipeline.)

### 5.2 Per-track SPS/PPS priming (critical)

H.264 over WebRTC carries SPS/PPS in each m-line's
`a=fmtp:<pt> … sprop-parameter-sets=…`, **not** necessarily in the IDR. Because
PlotJuggler's `StreamingVideoDecoder` scans the keyframe payload for parameter
sets, the receiver MUST inject the SPS/PPS in-band ahead of each keyframe.

With multi-track, **each m-line carries its own `sprop`**. Therefore the receiver
maintains **one `H264AnnexBNormalizer` per mid**, primed from that m-line's
`sprop-parameter-sets`. A single shared normalizer is incorrect once there is
more than one camera.

---

## 6. Message schemas

### 6.1 Handshake (bare strings — unchanged)

```
→  HELLO receiver          (receiver registers its peer id)
←  HELLO                   (broker ack)
→  SESSION robot           (optional: pair with a specific streamer peer id)
←  SESSION_OK              (broker ack)
←  ERROR <reason>          (any failure)
```

### 6.2 `list` — request the catalog (receiver → streamer/broker)

Optional; a streamer/broker MAY advertise unsolicited.

```json
{ "type": "list", "protocol": 1 }
```

### 6.3 `catalog` — advertise streams (streamer/broker → receiver)

```json
{
  "type": "catalog",
  "protocol": 1,
  "streams": [
    { "id": "cam0", "name": "camera0", "codec": "h264", "width": 640, "height": 480, "mid": "cam0" },
    { "id": "cam1", "name": "camera1", "codec": "h264", "width": 640, "height": 480, "mid": "cam1" }
  ]
}
```

| Field | Req? | Meaning |
|-------|------|---------|
| `id` | **required** | stable stream id; the value used in `subscribe` and as `a=mid` |
| `name` | optional | human label for the dialog and the topic leaf (defaults to `id`) |
| `codec` | optional | only `"h264"` is selectable in protocol 1 |
| `width` | optional | px, `0`/absent if unknown |
| `height` | optional | px, `0`/absent if unknown |
| `mid` | optional | the mid the streamer WILL assign; if present it **MUST equal `id`**; if absent it defaults to `id` |

### 6.4 `subscribe` — select streams (receiver → streamer/broker)

```json
{ "type": "subscribe", "protocol": 1, "streams": ["cam0", "cam2"] }
```

An empty list is invalid; the receiver MUST NOT send `subscribe` with zero
streams (OK is gated on a non-empty selection in the dialog, or a manual id).

### 6.5 `sdp` — description relay (both directions)

Legacy wire form; **no `type`/`protocol` envelope** so legacy senders
interoperate. Receiver only ever sends `"answer"`; streamer only ever `"offer"`.

```json
{ "sdp": { "type": "offer",  "sdp": "v=0\r\n… m=video … a=mid:cam0 … m=video … a=mid:cam2 …" } }
{ "sdp": { "type": "answer", "sdp": "v=0\r\n… a=mid:cam0 … a=mid:cam2 …" } }
```

### 6.6 `ice` — trickle ICE relay (both directions)

```json
{ "ice": { "candidate": "candidate:… typ host", "sdpMLineIndex": 0 } }
```

- `candidate` — the SDP candidate line.
- `sdpMLineIndex` — the zero-based m-line index the candidate belongs to. The
  receiver maps it back to the owning mid via the ordered `mline_index → mid`
  table built when the offer is applied. With BUNDLE (`max-bundle`) candidates
  apply to the whole bundle, so the index is sufficient.
- `sdpMid` — **optional / ignored.** Neither end emits or reads it today; routing
  is by `sdpMLineIndex`. A sender MAY include it for interoperability with other
  receivers, but a conforming receiver here MUST NOT depend on it.

### 6.7 `ERROR` semantics summary

| Error | Sent by | Meaning / receiver action |
|-------|---------|---------------------------|
| `ERROR peer … not found` | broker | SESSION target absent → abort |
| `ERROR peer busy` | broker | target already in a session → abort |
| `ERROR not in a session` | broker | protocol misuse → abort |
| `ERROR no valid streams in subscribe` | broker | none of the subscribed ids exist → re-open the dialog |
| `ERROR unsupported protocol <n>` | streamer | subscribe version too new → fall back / warn |

---

## 7. Manual / legacy single-stream fallback

A sender is "legacy / non-advertising" if, after the handshake, it does **not**
emit a `catalog` and just offers media directly (what a stock GStreamer
`webrtc_sendrecv` peer does).

The plugin supports this with **zero protocol on the wire**:

1. The discovery dialog, finding no catalog within the timeout, presents a
   **manual entry**: the user types a single stream id (the mid they expect, or
   any label) and/or leaves it blank. If blank, the receiver uses the first video
   m-line of whatever offer arrives.
2. The receiver does **not** send `list`/`subscribe` (or sends an empty-driven
   no-op). It waits for the offer, applies it, and:
   - one video m-line → that single track maps to the one configured topic
     (current single-stream behavior, byte-for-byte preserved);
   - multiple m-lines → each is still keyed by its `mid` and produces one topic.
3. A legacy `mid` may be a GStreamer-style label like `video0`. The receiver uses
   whatever `track->mid()` returns as the stream id; a manual stream id the user
   typed is used only for the topic name.

**The single-stream path is never regressed.** Multi-camera is a superset reached
only when both peers speak `protocol 1`.

---

## 8. C++-facing shape

All callbacks fire on a libdatachannel worker thread and must stay non-blocking.

### 8.1 Discovered-stream struct (`webrtc_signaling.hpp`, namespace `PJ::webrtc`)

```cpp
struct DiscoveredStream {
  std::string id;             // stable stream id; used in subscribe and as a=mid
  std::string name;           // human label / topic leaf (defaults to id)
  std::string codec = "h264"; // only "h264" is selectable in protocol 1
  int width = 0;              // advertised width in px (0 == unknown)
  int height = 0;             // advertised height in px (0 == unknown)
  std::string mid;            // == id; defaults to id when absent
};
```

### 8.2 Signaling API additions (`WebrtcSignaling`)

```cpp
using CatalogCallback = std::function<void(std::vector<DiscoveredStream>)>;
void setCatalogCallback(CatalogCallback cb);
void requestList();                                       // {"type":"list","protocol":1}
void subscribe(const std::vector<std::string>& stream_ids);// {"type":"subscribe",…}
std::vector<DiscoveredStream> discoveredStreams() const;  // thread-safe snapshot
```

### 8.3 `onMessage` dispatch additions

After HELLO/SESSION/ERROR handling and the JSON parse, branch on a new `type`
field **before** the legacy `sdp`/`ice` checks, so untyped legacy messages still
fall through unchanged:

```cpp
if (msg.contains("type") && msg["type"].is_string()) {
  if (msg["type"] == "catalog") { onCatalog(msg); return; }
  return;  // "list"/"subscribe" are receiver→streamer only; ignore echoes
}
// …existing legacy {"sdp":…} / {"ice":…} handling unchanged…
```

### 8.4 Receiver side

`WebrtcReceiver` generalizes from single-track to per-mid:

- `track_` → `std::map<std::string /*mid*/, TrackState>` (retain every track).
- one `H264AnnexBNormalizer` per mid, primed from that m-line's
  `sprop-parameter-sets`.
- `open(config, std::vector<StreamSpec> expected)`; `drainByStream()` returns
  `std::vector<std::pair<std::string /*stream_id==mid*/, EncodedFrame>>`.
- ICE candidate association uses an ordered `mline_index → mid` table.

The source calls `runtimeHost().ensureParserBinding` once per selected stream and
`video_emit::pushVideoFrame` per inbound `(mid, frame)` — one canonical
`PJ.VideoFrame` topic per camera.

---

## 9. Conformance checklist

A conforming **streamer** (protocol 1):

- [ ] completes HELLO/SESSION as the broker expects;
- [ ] sends a `catalog` (`protocol:1`) with one unique-`id` entry per camera;
- [ ] on `subscribe` builds **one** offer, one recvonly H.264 m-line per id,
      `a=mid:<id>` (+ `a=msid:<id> <id>`), `bundle-policy=max-bundle`;
- [ ] re-offers on re-subscribe;
- [ ] emits trickle ICE with the correct `sdpMLineIndex` for each candidate.

A conforming **receiver** (the plugin, always answerer):

- [ ] never offers; answers every offer, all m-lines recvonly;
- [ ] keys tracks by `track->mid()`, retains every track shared_ptr;
- [ ] primes one normalizer per mid from per-m-line `sprop-parameter-sets`;
- [ ] ignores unknown JSON fields; handles protocol-mismatch per §3.1;
- [ ] falls back to single-stream/manual mode without regressing the legacy path.
