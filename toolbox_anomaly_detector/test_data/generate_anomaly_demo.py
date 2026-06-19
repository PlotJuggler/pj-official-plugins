#!/usr/bin/env python3
"""Generate anomaly_demo.csv: a synthetic signal with injected anomalies so every
Anomaly Detector function has something to fire on.

Columns: time (s), value, clean (the anomaly-free reference).

Injected anomalies (over 0..20 s, dt = 0.01 s):
  - baseline      : 1 Hz sine, amplitude 0.4 (stays inside [-0.5, 0.5])
  - out-of-range  : a plateau at 1.2 over [5, 8) s        -> Out of range / Threshold
  - flatline      : value stuck at 0.3 over [12, 15) s    -> Flatline
  - step          : permanent +1.0 level shift from 16 s  -> Spike / Rate of change
  - incoherent #1 : single-sample outlier to  3.0 at 2 s  -> Incoherent point / Spike
  - incoherent #2 : single-sample outlier to -2.5 at 10 s -> Incoherent point / Spike
"""
import math

DT = 0.01
N = 2001  # 0 .. 20 s inclusive


def base_value(t: float) -> float:
    v = 0.4 * math.sin(2.0 * math.pi * 1.0 * t)
    if 5.0 <= t < 8.0:          # out-of-range plateau
        v = 1.2
    if 12.0 <= t < 15.0:        # flatline
        v = 0.3
    if t >= 16.0:               # permanent step
        v += 1.0
    return v


def idx(t: float) -> int:
    return int(round(t / DT))


def main() -> None:
    rows = []
    for k in range(N):
        t = round(k * DT, 5)
        clean = base_value(t)
        rows.append([t, clean, clean])

    # Single-sample outliers (break continuity) on the `value` column only.
    rows[idx(2.0)][1] = 3.0
    rows[idx(10.0)][1] = -2.5

    lines = ["time,value,clean"]
    for t, v, c in rows:
        lines.append(f"{t:.3f},{v:.5f},{c:.5f}")
    out = __file__.rsplit("/", 1)[0] + "/anomaly_demo.csv"
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {out} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
