#!/usr/bin/env python3
"""Publish sample LSL streams for testing data_stream_lsl.

Emits:
  - "PJTest" (EEG, 4 channels float32 @ 100 Hz) with named channels
  - "PJMarkers" (Markers, 1 channel string, irregular) with periodic events

Requires: pip install pylsl  (which bundles or finds liblsl)
"""
import math
import time

from pylsl import StreamInfo, StreamOutlet, cf_float32, cf_string


def make_numeric():
    info = StreamInfo("PJTest", "EEG", 4, 100.0, cf_float32, "pj-test-numeric")
    chns = info.desc().append_child("channels")
    for label in ("sine", "cosine", "sawtooth", "square"):
        chns.append_child("channel").append_child_value("label", label)
    return StreamOutlet(info)


def make_markers():
    info = StreamInfo("PJMarkers", "Markers", 1, 0.0, cf_string, "pj-test-markers")
    info.desc().append_child("channels").append_child("channel").append_child_value("label", "event")
    return StreamOutlet(info)


def main():
    numeric = make_numeric()
    markers = make_markers()
    print("Publishing PJTest (EEG) and PJMarkers (Markers). Ctrl-C to stop.")
    t0 = time.time()
    next_marker = t0 + 1.0
    n = 0
    while True:
        t = time.time() - t0
        numeric.push_sample([
            math.sin(2 * math.pi * t),
            math.cos(2 * math.pi * t),
            (t % 1.0),
            1.0 if (t % 1.0) < 0.5 else 0.0,
        ])
        now = time.time()
        if now >= next_marker:
            markers.push_sample([f"event_{n}"])
            n += 1
            next_marker = now + 1.0
        time.sleep(0.01)  # ~100 Hz


if __name__ == "__main__":
    main()
