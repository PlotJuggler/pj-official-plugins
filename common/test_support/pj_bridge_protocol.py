# Copyright 2026 PlotJuggler contributors
# SPDX-License-Identifier: MIT
"""Shared PJ Bridge control-plane helpers for the test servers.

The three test servers (pj_bridge_test_server, pj_bridge_mixed_server,
pj_bridge_mcap_player) speak one wire protocol; only their topic source and
data-emit loop differ. These pure helpers own the protocol-critical envelope
and the additive-subscribe response shape so a change lands in ONE place
instead of drifting across three near-identical copies.

Mirrors the production BridgeServer (github.com/PlotJuggler/plotjuggler_bridge):
protocol_version + id echo, additive subscribe with per-topic failures, the
optional `latched`/`include_schemas` topic-entry fields.
"""

PROTOCOL_VERSION = 1


def inject_response_fields(response: dict, request: dict) -> dict:
    """Attach protocol_version to every response and echo the request's `id`
    when it is a string (production BridgeServer::inject_response_fields)."""
    response["protocol_version"] = PROTOCOL_VERSION
    rid = request.get("id")
    if isinstance(rid, str):
        response["id"] = rid
    return response


def parse_topic_names(topics) -> list:
    """Extract topic names from a subscribe/unsubscribe `topics` array, which
    may mix plain strings and {"name": ..., "max_rate_hz": ...} objects
    (production accepts both; rate limits are ignored here)."""
    names = []
    if isinstance(topics, list):
        for item in topics:
            if isinstance(item, str):
                names.append(item)
            elif isinstance(item, dict) and isinstance(item.get("name"), str):
                names.append(item["name"])
    return names


def topic_entry(topic: dict, include_schemas: bool) -> dict:
    """One get_topics / topics_changed entry: {name, type} by default; adds
    {encoding, definition} when include_schemas was requested, and `latched:
    true` when the topic table marks it transient-local (production badge —
    absent means not latched or unknown)."""
    entry = {"name": topic["name"], "type": topic["type"]}
    if topic.get("latched"):
        entry["latched"] = True
    if include_schemas:
        entry["encoding"] = topic["encoding"]
        entry["definition"] = topic["definition"]
    return entry


def build_subscribe_response(requested_topics, known_by_name: dict, subscribed: set):
    """Build an ADDITIVE subscribe response and mutate `subscribed` in place.

    `known_by_name` maps topic name -> a dict carrying at least `encoding` and
    `definition`. Newly-subscribed topics are added to `subscribed` and their
    schemas returned; already-subscribed topics are a no-op (no schema
    re-echoed); unknown topics become per-topic failures the server forgets.

    Returns (response_dict, newly_subscribed_names) — the caller injects the
    envelope fields and (for the mcap player) replays latched samples for the
    newly-subscribed set.
    """
    requested = parse_topic_names(requested_topics)
    schemas = {}
    failures = []
    newly_subscribed = []
    for name in requested:
        if name in subscribed:
            continue
        topic = known_by_name.get(name)
        if topic is None:
            failures.append({"topic": name, "reason": "Topic does not exist"})
            continue
        subscribed.add(name)
        newly_subscribed.append(name)
        schemas[name] = {"encoding": topic["encoding"], "definition": topic["definition"]}

    resp: dict = {}
    if not failures:
        resp["status"] = "success"
    elif not schemas:
        resp["status"] = "error"
        resp["error_code"] = "ALL_SUBSCRIPTIONS_FAILED"
        resp["message"] = "Failed to subscribe to all requested topics"
    else:
        resp["status"] = "partial_success"
        resp["message"] = "Some subscriptions failed"
    resp["schemas"] = schemas
    if failures:
        resp["failures"] = failures
    return resp, newly_subscribed
