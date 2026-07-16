# Copyright 2026 PlotJuggler contributors
# SPDX-License-Identifier: MIT
"""Transient-local (latched) topic detection for MCAP replay test servers.

The ONE shared implementation used by both bridge test players
(data_stream_foxglove_bridge and data_stream_pj_bridge), mirroring how the
production servers decide it: the best available signal wins, and "unknown"
degrades to a conservative name heuristic rather than a false claim.

Signal precedence:
1. Recorded QoS — rosbag2 stores each channel's ``offered_qos_profiles`` in
   MCAP channel metadata. Present only in rosbag2-recorded bags; many real
   bags (other recorders, post-processed files) carry empty metadata.
2. Name heuristic — the conventional latched topics: ``*_static``
   (``/tf_static``), ``/map``, ``/robot_description``.
3. Explicit override — a per-invocation extra set (the players' ``--latch``
   CLI flag) for bags that fit neither.

Latched topics are replayed to late subscribers right after their subscribe
response — under demand-driven subscriptions EVERY subscriber is late by
construction, so without this replay a client can never resolve /tf_static's
static frames (one message per bag, at second zero).
"""

import re

_LATCHED_NAMES = ("/map", "/robot_description")


def is_latched_channel(topic: str, metadata: dict, extra_latched: set = frozenset()) -> bool:
    """True when `topic` should behave transient-local on replay.

    `metadata` is the MCAP channel metadata dict (may be empty).
    """
    if topic in extra_latched or topic.endswith("_static") or topic in _LATCHED_NAMES:
        return True
    qos = (metadata or {}).get("offered_qos_profiles", "").lower()
    # Covers both YAML vocabularies: "durability: 1" (old enum) and
    # "durability: transient_local" (jazzy+). The leading word boundary keeps
    # "durability: 1" from also matching "max_durability: 1".
    return "transient_local" in qos or re.search(r"\bdurability:\s*1\b", qos) is not None
