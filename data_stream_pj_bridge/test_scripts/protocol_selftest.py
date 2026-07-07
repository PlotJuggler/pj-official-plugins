#!/usr/bin/env python3
"""
PJ Bridge protocol self-test — a standalone WebSocket CLIENT that verifies a
running test server implements the PRODUCTION PJ Bridge control-plane semantics
(github.com/PlotJuggler/plotjuggler_bridge) end-to-end.

It is server-agnostic: it discovers topics via get_topics, then exercises the
protocol against whatever the server advertises. Run it against any of the three
test servers (pj_bridge_test_server.py, pj_bridge_mixed_server.py,
pj_bridge_mcap_player.py).

Checks (each PASS / FAIL / SKIP):
  * get_topics WITHOUT include_schemas → entries carry ONLY {name, type}
  * get_topics WITH  include_schemas   → entries additionally carry encoding+definition
  * heartbeat → {status:ok}; id echoed; protocol_version == 1
  * a request WITHOUT id → response omits id but still carries protocol_version
  * subscribe is ADDITIVE across two consecutive calls; both topics' data frames flow
  * unsubscribe stops that topic's data within a timeout while the other keeps flowing
  * unknown-topic subscribe → per-topic failure + partial_success
  * all-unknown subscribe   → status:error, error_code ALL_SUBSCRIPTIONS_FAILED
  * subscribe_topic_updates opt-in → {status:ok, topic_updates:true}
  * (optional) a late topic → topics_changed notification with schemas per the opt-in flag

Usage:
    python3 protocol_selftest.py [ws://HOST:PORT] [--host H] [--port P]
                                 [--expect-late-topic NAME] [--late-timeout SECONDS]

Exits nonzero if any check FAILs.
"""

import argparse
import asyncio
import json
import struct
import sys
import time

import websockets
import zstandard as zstd

MAGIC = 0x42524A50  # "PJRB" LE
BOGUS = "/__pj_selftest_nonexistent__"
BOGUS2 = "/__pj_selftest_nonexistent_2__"


class SkipCheck(Exception):
    """Raised by a check to record SKIP (not FAIL)."""


def need(cond, msg):
    if not cond:
        raise AssertionError(msg)


def decode_frame_topics(frame: bytes) -> list:
    """Extract the topic names from one PJRB binary data frame. Returns [] for a
    frame that is not a valid PJRB frame."""
    if len(frame) < 16:
        return []
    magic, count, usize, _flags = struct.unpack("<IIII", frame[:16])
    if magic != MAGIC:
        return []
    comp = frame[16:]
    try:
        payload = zstd.ZstdDecompressor().decompress(comp)
    except zstd.ZstdError:
        try:
            payload = zstd.ZstdDecompressor().decompress(comp, max_output_size=usize)
        except Exception:
            return []
    topics = []
    off, n = 0, len(payload)
    for _ in range(count):
        if off + 2 > n:
            break
        (tlen,) = struct.unpack_from("<H", payload, off)
        off += 2
        if off + tlen > n:
            break
        topic = payload[off:off + tlen].decode("utf-8", "replace")
        off += tlen
        if off + 8 > n:
            break
        off += 8  # timestamp_ns
        if off + 4 > n:
            break
        (clen,) = struct.unpack_from("<I", payload, off)
        off += 4 + clen
        topics.append(topic)
    return topics


class Conn:
    """WebSocket connection with a background reader that routes text responses to
    a queue, notifications to a list, and binary data frames to per-topic
    last-seen timestamps."""

    def __init__(self, ws):
        self.ws = ws
        self.responses: asyncio.Queue = asyncio.Queue()
        self.notifications: list = []
        self.topic_last_seen: dict = {}
        self.topic_count: dict = {}
        self._task = asyncio.create_task(self._reader())

    async def _reader(self):
        try:
            async for msg in self.ws:
                if isinstance(msg, (bytes, bytearray)):
                    now = time.monotonic()
                    for topic in decode_frame_topics(bytes(msg)):
                        self.topic_last_seen[topic] = now
                        self.topic_count[topic] = self.topic_count.get(topic, 0) + 1
                else:
                    try:
                        data = json.loads(msg)
                    except json.JSONDecodeError:
                        continue
                    if isinstance(data, dict) and data.get("notification"):
                        self.notifications.append(data)
                    else:
                        await self.responses.put(data)
        except websockets.exceptions.ConnectionClosed:
            pass

    async def request(self, obj: dict, timeout: float = 5.0) -> dict:
        await self.ws.send(json.dumps(obj))
        return await asyncio.wait_for(self.responses.get(), timeout)

    def seen_recently(self, topic: str, within: float) -> bool:
        ts = self.topic_last_seen.get(topic)
        return ts is not None and (time.monotonic() - ts) <= within

    async def close(self):
        self._task.cancel()
        try:
            await self.ws.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Individual checks (return a detail string, or raise AssertionError / SkipCheck)
# ---------------------------------------------------------------------------

def assert_response_envelope(resp: dict, expect_id):
    need(resp.get("protocol_version") == 1,
         f"protocol_version != 1 (got {resp.get('protocol_version')!r})")
    if expect_id is None:
        need("id" not in resp, f"id echoed when request had none: {resp.get('id')!r}")
    else:
        need(resp.get("id") == expect_id, f"id not echoed (want {expect_id!r}, got {resp.get('id')!r})")


async def check_get_topics_no_schemas(conn, ctx):
    resp = await conn.request({"command": "get_topics", "id": "gt-noflag"})
    assert_response_envelope(resp, "gt-noflag")
    need(resp.get("status") == "success", f"status != success: {resp.get('status')!r}")
    topics = resp.get("topics")
    need(isinstance(topics, list) and topics, "topics missing/empty")
    for e in topics:
        need("name" in e and "type" in e, f"entry missing name/type: {e}")
        need("encoding" not in e and "definition" not in e,
             f"default get_topics leaked schema fields: {e}")
    names = [e["name"] for e in topics]
    ctx["topics"] = names
    return f"{len(names)} topics, name+type only"


async def check_get_topics_with_schemas(conn, ctx):
    resp = await conn.request({"command": "get_topics", "id": "gt-flag", "include_schemas": True})
    assert_response_envelope(resp, "gt-flag")
    need(resp.get("status") == "success", f"status != success: {resp.get('status')!r}")
    topics = resp.get("topics") or []
    need(topics, "topics missing/empty")
    for e in topics:
        need("encoding" in e and "definition" in e,
             f"include_schemas entry missing encoding/definition: {e}")
    return f"{len(topics)} topics carry encoding+definition"


async def check_heartbeat_echo(conn, ctx):
    resp = await conn.request({"command": "heartbeat", "id": "hb-1"})
    assert_response_envelope(resp, "hb-1")
    need(resp.get("status") == "ok", f"heartbeat status != ok: {resp.get('status')!r}")
    return "status ok, id echoed, protocol_version 1"


async def check_id_omitted_when_absent(conn, ctx):
    resp = await conn.request({"command": "heartbeat"})
    assert_response_envelope(resp, None)
    need(resp.get("status") == "ok", f"status != ok: {resp.get('status')!r}")
    return "no id key; protocol_version still present"


async def check_topic_updates_optin(conn, ctx):
    # Opt in EARLY (before the data-flow sleeps) so a late-topic push is captured.
    resp = await conn.request(
        {"command": "subscribe_topic_updates", "id": "tu-1", "include_schemas": True})
    assert_response_envelope(resp, "tu-1")
    need(resp.get("status") == "ok", f"status != ok: {resp.get('status')!r}")
    need(resp.get("topic_updates") is True, f"topic_updates != true: {resp.get('topic_updates')!r}")
    return "opted in (include_schemas=true)"


def _two_topics(ctx):
    names = ctx.get("topics") or []
    if len(names) < 2:
        raise SkipCheck(f"server advertises <2 topics ({names})")
    return names[0], names[1]


async def check_subscribe_additive(conn, ctx):
    t0, t1 = _two_topics(ctx)
    r0 = await conn.request({"command": "subscribe", "id": "sub-a", "topics": [t0]})
    assert_response_envelope(r0, "sub-a")
    need(r0.get("status") == "success", f"first subscribe status != success: {r0.get('status')!r}")
    need(t0 in (r0.get("schemas") or {}), f"schemas missing {t0}: {r0.get('schemas')}")
    need("encoding" in r0["schemas"][t0] and "definition" in r0["schemas"][t0],
         f"schema entry missing encoding/definition: {r0['schemas'][t0]}")

    r1 = await conn.request({"command": "subscribe", "id": "sub-b", "topics": [t1]})
    assert_response_envelope(r1, "sub-b")
    need(r1.get("status") == "success", f"second subscribe status != success: {r1.get('status')!r}")
    need(t1 in (r1.get("schemas") or {}), f"schemas missing {t1}: {r1.get('schemas')}")
    ctx["t0"], ctx["t1"] = t0, t1
    return f"subscribed {t0} then {t1} (additive)"


async def check_additive_data_flow(conn, ctx):
    t0, t1 = ctx.get("t0"), ctx.get("t1")
    need(t0 and t1, "prior additive-subscribe check did not run")
    await asyncio.sleep(1.5)
    need(conn.seen_recently(t0, 0.9), f"no recent data for {t0} (count={conn.topic_count.get(t0, 0)})")
    need(conn.seen_recently(t1, 0.9), f"no recent data for {t1} (count={conn.topic_count.get(t1, 0)})")
    return f"both flowing ({t0}:{conn.topic_count.get(t0,0)}, {t1}:{conn.topic_count.get(t1,0)} frames)"


async def check_unsubscribe_stops_flow(conn, ctx):
    t0, t1 = ctx.get("t0"), ctx.get("t1")
    need(t0 and t1, "prior additive-subscribe check did not run")
    resp = await conn.request({"command": "unsubscribe", "id": "unsub-a", "topics": [t0]})
    assert_response_envelope(resp, "unsub-a")
    need(resp.get("status") == "success", f"unsubscribe status != success: {resp.get('status')!r}")
    need(resp.get("removed") == [t0], f"removed != [{t0}]: {resp.get('removed')!r}")
    t_unsub = time.monotonic()
    await asyncio.sleep(1.0)
    last_t0 = conn.topic_last_seen.get(t0, 0.0)
    need(last_t0 <= t_unsub + 0.5, f"{t0} still streaming after unsubscribe")
    need(conn.seen_recently(t1, 0.9), f"{t1} stopped flowing after unrelated unsubscribe")
    return f"{t0} stopped; {t1} still flowing"


async def check_unknown_topic_partial_success(conn, ctx):
    t0 = ctx.get("t0")
    need(t0, "prior checks did not establish a known topic")
    # t0 was unsubscribed in the previous check, so it is a fresh (not-yet-subscribed)
    # known topic → mixing it with a bogus one yields partial_success.
    resp = await conn.request({"command": "subscribe", "id": "sub-p", "topics": [t0, BOGUS]})
    assert_response_envelope(resp, "sub-p")
    need(resp.get("status") == "partial_success", f"status != partial_success: {resp.get('status')!r}")
    need(t0 in (resp.get("schemas") or {}), f"schemas missing surviving topic {t0}: {resp.get('schemas')}")
    failures = resp.get("failures") or []
    hit = [f for f in failures if f.get("topic") == BOGUS]
    need(hit, f"no per-topic failure for {BOGUS}: {failures}")
    need(hit[0].get("reason") == "Topic does not exist",
         f"unexpected failure reason: {hit[0].get('reason')!r}")
    return f"partial_success; failure reason={hit[0].get('reason')!r}"


async def check_all_failed_error(conn, ctx):
    resp = await conn.request({"command": "subscribe", "id": "sub-e", "topics": [BOGUS, BOGUS2]})
    assert_response_envelope(resp, "sub-e")
    need(resp.get("status") == "error", f"status != error: {resp.get('status')!r}")
    need(resp.get("error_code") == "ALL_SUBSCRIPTIONS_FAILED",
         f"error_code != ALL_SUBSCRIPTIONS_FAILED: {resp.get('error_code')!r}")
    return "status error, ALL_SUBSCRIPTIONS_FAILED"


async def check_late_topic_notification(conn, ctx, expect_late_topic, late_timeout):
    if not expect_late_topic:
        raise SkipCheck("no --expect-late-topic (server pushes no topics_changed)")

    def find():
        for note in conn.notifications:
            if note.get("notification") != "topics_changed":
                continue
            for entry in note.get("added", []):
                if entry.get("name") == expect_late_topic:
                    return note, entry
        return None, None

    deadline = time.monotonic() + late_timeout
    note, entry = find()
    while entry is None and time.monotonic() < deadline:
        await asyncio.sleep(0.1)
        note, entry = find()

    need(entry is not None,
         f"no topics_changed naming {expect_late_topic} within {late_timeout}s")
    need("id" not in note, f"notification carried an id: {note.get('id')!r}")
    need(note.get("protocol_version") == 1,
         f"notification protocol_version != 1: {note.get('protocol_version')!r}")
    # We opted in with include_schemas=true, so added entries must carry schemas.
    need("encoding" in entry and "definition" in entry,
         f"opted in with include_schemas=true but added entry lacks schema fields: {entry}")
    return f"topics_changed added {expect_late_topic} with schemas"


CHECKS = [
    ("get_topics_no_schemas", check_get_topics_no_schemas),
    ("get_topics_with_schemas", check_get_topics_with_schemas),
    ("heartbeat_echo", check_heartbeat_echo),
    ("id_omitted_when_absent", check_id_omitted_when_absent),
    ("subscribe_topic_updates_optin", check_topic_updates_optin),
    ("subscribe_additive", check_subscribe_additive),
    ("additive_data_flow", check_additive_data_flow),
    ("unsubscribe_stops_flow", check_unsubscribe_stops_flow),
    ("unknown_topic_partial_success", check_unknown_topic_partial_success),
    ("all_failed_error", check_all_failed_error),
]


async def run(url: str, expect_late_topic, late_timeout: float) -> int:
    results = []
    ctx: dict = {}
    async with websockets.connect(url, max_size=None) as ws:
        conn = Conn(ws)
        try:
            for name, fn in CHECKS:
                try:
                    detail = await fn(conn, ctx)
                    results.append((name, "PASS", detail or ""))
                except SkipCheck as e:
                    results.append((name, "SKIP", str(e)))
                except AssertionError as e:
                    results.append((name, "FAIL", str(e)))
                except Exception as e:  # noqa: BLE001 — any error is a failed check
                    results.append((name, "FAIL", f"{type(e).__name__}: {e}"))

            # Late-topic check runs last so the opt-in above had time to catch a push.
            try:
                detail = await check_late_topic_notification(
                    conn, ctx, expect_late_topic, late_timeout)
                results.append(("late_topic_notification", "PASS", detail))
            except SkipCheck as e:
                results.append(("late_topic_notification", "SKIP", str(e)))
            except AssertionError as e:
                results.append(("late_topic_notification", "FAIL", str(e)))
            except Exception as e:  # noqa: BLE001
                results.append(("late_topic_notification", "FAIL", f"{type(e).__name__}: {e}"))
        finally:
            await conn.close()

    print(f"\n=== protocol_selftest — {url} ===")
    for name, status, detail in results:
        print(f"  [{status:4}] {name}{('  — ' + detail) if detail else ''}")
    npass = sum(1 for _, s, _ in results if s == "PASS")
    nfail = sum(1 for _, s, _ in results if s == "FAIL")
    nskip = sum(1 for _, s, _ in results if s == "SKIP")
    print(f"\n{npass} passed, {nfail} failed, {nskip} skipped")
    return 1 if nfail else 0


def main():
    parser = argparse.ArgumentParser(description="PJ Bridge protocol self-test client")
    parser.add_argument("url", nargs="?", default=None, help="ws://HOST:PORT (default ws://<host>:<port>)")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=9871)
    parser.add_argument("--expect-late-topic", default=None,
                        help="Topic name the server advertises late (test server --late-topic)")
    parser.add_argument("--late-timeout", type=float, default=8.0,
                        help="Seconds to wait for the topics_changed push (default 8)")
    args = parser.parse_args()

    url = args.url or f"ws://{args.host}:{args.port}"
    try:
        rc = asyncio.run(run(url, args.expect_late_topic, args.late_timeout))
    except KeyboardInterrupt:
        rc = 130
    sys.exit(rc)


if __name__ == "__main__":
    main()
