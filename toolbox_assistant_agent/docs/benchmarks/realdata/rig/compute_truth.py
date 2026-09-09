#!/usr/bin/env python3
"""Independent ground-truth computation for the realdata benchmark.

Uses pyulog / rosbags / mcap / pandas directly on the ORIGINAL source data
(never anything from the assistant plugin) to produce one JSON file per
task/file under truth/. Run inside the rig venv:

  /home/alvvm/Work/assistant-realdata-bench/.venv/bin/python \
      /home/alvvm/Work/assistant-realdata-bench/truth/compute_truth.py
"""
import json
import math
import os

import numpy as np
import pandas as pd
import pyulog
from rosbags.highlevel import AnyReader

ROOT = "/home/alvvm/Work/assistant-realdata-bench"
TRUTH_DIR = os.path.join(ROOT, "truth")
PRIVATE_MAP_PATH = os.path.join(TRUTH_DIR, "private_map.json")


def write_truth(name, obj):
    path = os.path.join(TRUTH_DIR, name + ".json")
    with open(path, "w") as f:
        json.dump(obj, f, indent=2, default=_json_default)
    print("wrote", path)


def _json_default(o):
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.floating,)):
        return float(o)
    if isinstance(o, (np.bool_,)):
        return bool(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    raise TypeError(f"not serializable: {type(o)}")


# ---------------------------------------------------------------------------
# PX4 (T01, T02, T03)
# ---------------------------------------------------------------------------

# PX4 nav_state (vehicle_status.msg) enum. Originally sourced against firmware
# ~v1.11 (the old bundled test fixture, kept as datasets/px4/_old_sample_hop.ulg,
# had ver_sw_branch v1.11.2_w_rc_sysid); the current flight.ulg is v1.15.4.
# The low-numbered entries used below (MANUAL/ALTCTL/POSCTL/AUTO_MISSION/
# AUTO_LOITER/AUTO_RTL) are original PX4 nav_states unchanged since v1.0, so
# this table is still valid for both firmware eras.
PX4_NAV_STATE_NAMES = {
    0: "MANUAL",
    1: "ALTCTL",
    2: "POSCTL",
    3: "AUTO_MISSION",
    4: "AUTO_LOITER",
    5: "AUTO_RTL",
    6: "AUTO_RCRECOVER",
    7: "AUTO_RTGS",
    8: "AUTO_LANDENGFAIL",
    9: "AUTO_LANDGPSFAIL",
    10: "ACRO",
    11: "UNUSED",
    12: "DESCEND",
    13: "TERMINATION",
    14: "OFFBOARD",
    15: "STAB",
    16: "RATTITUDE",
    17: "AUTO_TAKEOFF",
    18: "AUTO_LAND",
    19: "AUTO_FOLLOW_TARGET",
    20: "AUTO_PRECLAND",
    21: "ORBIT",
    22: "AUTO_VTOL_TAKEOFF",
}


def px4_ulog():
    return pyulog.ULog(os.path.join(ROOT, "datasets/px4/flight.ulg"))


def get_dataset(u, name, multi_id=0):
    for d in u.data_list:
        if d.name == name and d.multi_id == multi_id:
            return d
    raise KeyError((name, multi_id))


def px4_time_axis(u):
    return {
        "unit": "s",
        "kind": "ulog_us_since_boot",
        "first_sample_abs": u.start_timestamp / 1e6,
        "last_sample_abs": u.last_timestamp / 1e6,
    }


def compute_t01(u):
    land = get_dataset(u, "vehicle_land_detected")
    ts = land.data["timestamp"] / 1e6
    landed = land.data["landed"].astype(int)

    events = []
    for i in range(1, len(landed)):
        if landed[i - 1] == 1 and landed[i] == 0:
            events.append({"t_abs": float(ts[i]), "kind": "takeoff", "detail": "vehicle_land_detected.landed 1->0", "tolerance_s": 1.0})
        elif landed[i - 1] == 0 and landed[i] == 1:
            events.append({"t_abs": float(ts[i]), "kind": "landing", "detail": "vehicle_land_detected.landed 0->1", "tolerance_s": 1.0})

    # Altitude candidates
    air = get_dataset(u, "vehicle_air_data")
    baro = air.data["baro_alt_meter"]
    baro_rel = baro - baro[0]
    max_baro = float(np.max(baro_rel))

    gps = get_dataset(u, "vehicle_gps_position")
    # PX4 changed vehicle_gps_position's altitude field across firmware
    # versions: older logs (<=~v1.13) carry integer "alt" in millimeters,
    # newer ones (>=~v1.14/1.15) carry float "altitude_msl_m" in meters
    # directly. Support both so this script works on logs from either era.
    if "alt" in gps.data:
        gps_alt_source = "vehicle_gps_position/alt (relative to first sample)"
        gps_alt_m = gps.data["alt"] / 1000.0
    else:
        gps_alt_source = "vehicle_gps_position/altitude_msl_m (relative to first sample)"
        gps_alt_m = gps.data["altitude_msl_m"]
    gps_alt_rel = gps_alt_m - gps_alt_m[0]
    max_gps = float(np.max(gps_alt_rel))

    lpos = get_dataset(u, "vehicle_local_position")
    alt_local = -lpos.data["z"]
    max_local = float(np.max(alt_local))

    # Ground speed candidates
    speed_local = np.sqrt(lpos.data["vx"] ** 2 + lpos.data["vy"] ** 2)
    max_speed_local = float(np.max(speed_local))
    max_speed_gps = float(np.max(gps.data["vel_m_s"]))

    return {
        "task": "T01",
        "file": "px4/flight",
        "time_axis": px4_time_axis(u),
        "events": events,
        "values": {
            "takeoff_s": {"candidates": [{"value": e["t_abs"], "source": "vehicle_land_detected.landed"} for e in events if e["kind"] == "takeoff"]},
            "landing_s": {"candidates": [{"value": e["t_abs"], "source": "vehicle_land_detected.landed"} for e in events if e["kind"] == "landing"]},
            "max_altitude_m": {
                "candidates": [
                    {"value": max_baro, "source": "vehicle_air_data/baro_alt_meter (relative to first sample)"},
                    {"value": max_gps, "source": gps_alt_source},
                    {"value": max_local, "source": "vehicle_local_position/-z"},
                ],
                "tolerance": 1.0,
            },
            "max_ground_speed_mps": {
                "candidates": [
                    {"value": max_speed_local, "source": "vehicle_local_position sqrt(vx^2+vy^2)"},
                    {"value": max_speed_gps, "source": "vehicle_gps_position/vel_m_s"},
                ],
                "tolerance": 0.5,
            },
        },
        "notes": (
            f"vehicle_land_detected carries {len(landed)} samples for the whole file, spanning "
            f"the file's own clock from ~{float(ts[0]):.1f}s to ~{float(ts[-1]):.1f}s "
            f"({len(events)} landed-state transition(s) found: "
            + ", ".join(f"{e['kind']}@{e['t_abs']:.1f}s" for e in events) + "). "
            "Altitude/speed tolerances are as specified in the task brief "
            "(1.0 s for times, 1.0 m, 0.5 m/s)."
        ),
    }


def compute_t02(u):
    st = get_dataset(u, "vehicle_status")
    ts = st.data["timestamp"] / 1e6
    nav = st.data["nav_state"].astype(int)

    events = []
    if len(nav) > 0:
        events.append({
            "t_abs": float(ts[0]),
            "kind": "mode_change",
            "detail": f"initial nav_state={int(nav[0])} ({PX4_NAV_STATE_NAMES.get(int(nav[0]), 'UNKNOWN')})",
            "tolerance_s": 0.5,
        })
    for i in range(1, len(nav)):
        if nav[i] != nav[i - 1]:
            events.append({
                "t_abs": float(ts[i]),
                "kind": "mode_change",
                "detail": f"nav_state {int(nav[i-1])}->{int(nav[i])} ({PX4_NAV_STATE_NAMES.get(int(nav[i]), 'UNKNOWN')})",
                "tolerance_s": 0.5,
            })

    changes_only = [e for e in events if "->" in e["detail"]]

    return {
        "task": "T02",
        "file": "px4/flight",
        "time_axis": px4_time_axis(u),
        "events": events,
        "values": {
            "num_mode_changes": {"value": len(changes_only), "unit": "count", "source": "vehicle_status.nav_state"},
        },
        "notes": (
            f"vehicle_status carries {len(nav)} samples for the whole file with "
            f"{len(changes_only)} nav_state change(s). PX4 nav_state name mapping is hardcoded "
            "from the firmware enum (vehicle_status.msg); pyulog itself does not expose enum "
            "label strings for this field."
        ),
    }


def _find_intervals(mask, ts, min_dur=0.1):
    """Contiguous True runs in `mask`, as (t_start, t_end, duration_s), keeping
    only runs whose span is >= min_dur (a lone matching sample has duration 0
    and is dropped, per the task brief: 'Report intervals >= 0.1 s')."""
    intervals = []
    n = len(mask)
    i = 0
    while i < n:
        if mask[i]:
            j = i
            while j + 1 < n and mask[j + 1]:
                j += 1
            dur = float(ts[j] - ts[i])
            if dur >= min_dur:
                intervals.append((float(ts[i]), float(ts[j]), dur))
            i = j + 1
        else:
            i += 1
    return intervals


def compute_t03(u):
    events = []
    channel_summaries = {}

    for inst in (0, 1):
        try:
            d = get_dataset(u, "actuator_outputs", inst)
        except KeyError:
            continue
        ts = d.data["timestamp"] / 1e6
        noutputs = int(d.data["noutputs"][0]) if len(d.data["noutputs"]) else 0
        for ch in range(min(noutputs, 16)):
            col = d.data[f"output[{ch}]"].astype(float)
            if len(col) == 0:
                continue
            obs_min, obs_max = float(col.min()), float(col.max())
            if obs_min == obs_max:
                # Flatlined channel (const output for the whole file) -- report but
                # flag specially, it is not a transient "saturation event".
                key = f"actuator_outputs[{inst}].output[{ch}]"
                channel_summaries[key] = {
                    "observed_min": obs_min, "observed_max": obs_max,
                    "flatlined": True,
                    "near_pwm_limit": obs_min in (1000.0, 2000.0),
                }
                continue
            key = f"actuator_outputs[{inst}].output[{ch}]"
            channel_summaries[key] = {"observed_min": obs_min, "observed_max": obs_max, "flatlined": False}

            for bound_name, bound_val in (("min", obs_min), ("max", obs_max)):
                mask = col == bound_val
                for (t0, t1, dur) in _find_intervals(mask, ts, min_dur=0.1):
                    events.append({
                        "t_abs": t0,
                        "t_end_abs": t1,
                        "kind": "actuator_saturation",
                        "detail": f"actuator_outputs[{inst}].output[{ch}] pinned at observed {bound_name} ({bound_val:.1f}) for {dur:.2f}s",
                        "source": f"actuator_outputs[{inst}]/output[{ch}]",
                        "tolerance_s": 0.5,
                    })
            # Also check literal PX4 PWM limits 1000/2000 if values are in PWM range
            # (skip a limit that coincides with this channel's own observed min/max,
            # already handled above -- avoids reporting the same interval twice).
            if 800 <= obs_min <= 2200 and 800 <= obs_max <= 2200:
                for bound_val in (1000.0, 2000.0):
                    if bound_val in (obs_min, obs_max):
                        continue
                    mask = col == bound_val
                    if mask.any():
                        for (t0, t1, dur) in _find_intervals(mask, ts, min_dur=0.1):
                            events.append({
                                "t_abs": t0,
                                "t_end_abs": t1,
                                "kind": "actuator_saturation",
                                "detail": f"actuator_outputs[{inst}].output[{ch}] pinned at PX4 PWM limit {bound_val:.0f} for {dur:.2f}s",
                                "source": f"actuator_outputs[{inst}]/output[{ch}]",
                                "tolerance_s": 0.5,
                            })

    for group in (0, 1):
        try:
            d = get_dataset(u, f"actuator_controls_{group}", 0)
        except KeyError:
            continue
        ts = d.data["timestamp"] / 1e6
        for ch in range(8):
            col = d.data[f"control[{ch}]"].astype(float)
            if len(col) == 0 or float(col.min()) == float(col.max()) == 0.0:
                continue
            for bound_val in (1.0, -1.0):
                mask = np.isclose(col, bound_val, atol=1e-6)
                if mask.any():
                    for (t0, t1, dur) in _find_intervals(mask, ts, min_dur=0.1):
                        events.append({
                            "t_abs": t0,
                            "t_end_abs": t1,
                            "kind": "actuator_saturation",
                            "detail": f"actuator_controls_{group}.control[{ch}] pinned at {bound_val:+.1f} for {dur:.2f}s",
                            "source": f"actuator_controls_{group}/control[{ch}]",
                            "tolerance_s": 0.5,
                        })

    if not events:
        events.append({"t_abs": None, "kind": "none", "detail": "no actuator channel observed pinned at its min/max (or PWM 1000/2000, or control +-1.0) for >= 0.1s", "tolerance_s": 0.5})

    u_ = u
    return {
        "task": "T03",
        "file": "px4/flight",
        "time_axis": px4_time_axis(u_),
        "events": events,
        "values": {"channel_observed_ranges": channel_summaries},
        "notes": (
            f"{len(events)} saturation-candidate event(s) found across "
            f"{len(channel_summaries)} observed actuator channel(s) "
            f"({sum(1 for v in channel_summaries.values() if v.get('flatlined'))} flatlined for "
            "the whole file, reported in values.channel_observed_ranges with flatlined=true but "
            "EXCLUDED from the saturation events list -- a flatlined channel usually means an "
            "inactive/unused output group (e.g. no ESCs wired to it) rather than a genuine "
            "in-flight saturation event, even though technically 'pinned at its own min==max'. "
            "Reference-only, not a hard grading oracle: whether a given channel's static or "
            "clipped range should count as 'saturating' is itself an interpretive judgment call "
            "for the model being tested."
        ),
    }


# ---------------------------------------------------------------------------
# ALFA (T07 / T08)
# ---------------------------------------------------------------------------

FAILURE_TOPIC_PREFIX = "/failure_status/"


def alfa_truth(alfa_id, meta):
    seq = meta["original_sequence"]
    bag_path = os.path.join(ROOT, "datasets/_src/alfa/bags", seq + ".bag")
    with AnyReader([__import__("pathlib").Path(bag_path)]) as reader:
        start_abs = reader.start_time / 1e9
        end_abs = reader.end_time / 1e9
        fault_conns = [c for c in reader.connections if c.topic.startswith(FAILURE_TOPIC_PREFIX)]
        t_fault = None
        fault_topics_seen = []
        if fault_conns:
            msgs = sorted(reader.messages(connections=fault_conns), key=lambda m: m[1])
            t_fault = msgs[0][1] / 1e9
            fault_topics_seen = sorted({c.topic for c in fault_conns})

    kind = meta["kind"]
    events = []
    if kind == "no_fault":
        events.append({"t_abs": None, "kind": "no_fault", "detail": meta["detail"], "tolerance_s": 2.0})
    else:
        events.append({
            "t_abs": t_fault,
            "kind": kind,
            "detail": meta["detail"],
            "source": ",".join(fault_topics_seen) if fault_topics_seen else None,
            "tolerance_s": 2.0,
        })

    truth = {
        "task": "T07",
        "file": f"alfa/{alfa_id}",
        "time_axis": {
            "unit": "s",
            "kind": "ros_epoch",
            "first_sample_abs": start_abs,
            "last_sample_abs": end_abs,
        },
        "events": events,
        "values": {
            "post_fault_end_abs": {"value": end_abs, "unit": "s", "source": "bag end time (ros epoch)"},
        },
        "notes": (
            "Fault time = timestamp of the first message on the failure_status/* topic(s) in the "
            "ORIGINAL bag (that topic is excluded from the shipped MCAP). Per the ALFA paper, the "
            "ground-truth topic is only published once the fault is active and is recorded at ~5Hz, "
            "so the true fault onset can be up to ~0.2s earlier than t_abs. The original sequence "
            "name is recorded only in truth/private_map.json, never here."
        ),
    }
    write_truth(f"T07_{alfa_id}", truth)
    truth_t08 = dict(truth)
    truth_t08["task"] = "T08"
    write_truth(f"T08_{alfa_id}", truth_t08)


# ---------------------------------------------------------------------------
# SKAB (T09 / T10)
# ---------------------------------------------------------------------------

def skab_truth(skab_id, meta):
    orig_rel = meta["original_file"]
    src = os.path.join(ROOT, "datasets/_src/SKAB/data", orig_rel)
    df = pd.read_csv(src, sep=";", parse_dates=["datetime"])
    df = df.sort_values("datetime").reset_index(drop=True)
    # Unit-agnostic epoch-seconds conversion (pandas' datetime64 storage unit
    # varies by version -- ns historically, us/s in newer pandas -- so do NOT
    # assume astype('int64') is nanoseconds).
    t_epoch = (df["datetime"] - pd.Timestamp("1970-01-01")) / pd.Timedelta(seconds=1)

    anomaly = df["anomaly"].astype(int).values
    idx_on = np.where((anomaly == 1) & (np.concatenate(([0], anomaly[:-1])) == 0))[0]
    idx_off_end = np.where((anomaly == 1) & (np.concatenate((anomaly[1:], [0])) == 0))[0]

    events = []
    sensor_cols = [c for c in df.columns if c not in ("datetime", "anomaly", "changepoint")]
    for on_i, off_i in zip(idx_on, idx_off_end):
        t_start = float(t_epoch.iloc[on_i])
        t_end = float(t_epoch.iloc[off_i])
        dur = t_end - t_start
        tol = max(5.0, 0.10 * dur)
        events.append({
            "t_abs": t_start,
            "t_end_abs": t_end,
            "kind": "anomaly",
            "detail": "labelled anomaly interval",
            "source": "anomaly column (removed from shipped CSV)",
            "tolerance_s": tol,
        })

    task = meta["task"]
    values = {}

    if task == "T10" and len(idx_on) == 1:
        on_i = idx_on[0]
        t_start = float(t_epoch.iloc[on_i])
        baseline = df.iloc[:on_i]
        first_dev = {}
        for col in sensor_cols:
            base_vals = baseline[col].astype(float).values
            if len(base_vals) < 5:
                continue
            mu, sigma = float(np.mean(base_vals)), float(np.std(base_vals))
            if sigma == 0:
                continue
            post = df[col].astype(float).values
            post_t = t_epoch.values
            dev_mask = np.abs(post - mu) > 3 * sigma
            # first index >= on_i with >=3 consecutive deviating samples
            found = None
            i = on_i
            n = len(dev_mask)
            while i < n - 2:
                if dev_mask[i] and dev_mask[i + 1] and dev_mask[i + 2]:
                    found = i
                    break
                i += 1
            if found is not None:
                first_dev[col] = {
                    "t_abs": float(post_t[found]),
                    "lead_s_after_anomaly_start": float(post_t[found] - t_start),
                    "baseline_mean": mu,
                    "baseline_std": sigma,
                }
        ordered = sorted(first_dev.items(), key=lambda kv: kv[1]["t_abs"])
        values["first_deviation_order"] = [
            {"sensor": k, **v} for k, v in ordered
        ]

    truth = {
        "task": task,
        "file": f"skab/{skab_id}",
        "time_axis": {
            "unit": "s",
            "kind": "csv_datetime_utc",
            "first_sample_abs": float(t_epoch.iloc[0]),
            "last_sample_abs": float(t_epoch.iloc[-1]),
        },
        "events": events,
        "values": values,
        "notes": (
            "datetime column parsed as naive timestamps (SKAB does not specify a timezone; treated "
            "as UTC epoch seconds for t_abs). The original SKAB file path is recorded only in "
            "truth/private_map.json, never here. "
            + ("first_deviation_order is a REFERENCE heuristic (>3 baseline-sigma for >=3 consecutive "
               "samples), not a hard ground truth -- different reasonable thresholds could reorder "
               "close sensors." if task == "T10" else "")
        ),
    }
    write_truth(f"{task}_{skab_id}", truth)


def main():
    os.makedirs(TRUTH_DIR, exist_ok=True)

    u = px4_ulog()
    t01 = compute_t01(u)
    t02 = compute_t02(u)
    t03 = compute_t03(u)
    write_truth("T01_px4", t01)
    write_truth("T02_px4", t02)
    write_truth("T03_px4", t03)

    pm = json.load(open(PRIVATE_MAP_PATH))
    for alfa_id, meta in sorted(pm.get("alfa", {}).items()):
        alfa_truth(alfa_id, meta)
    for skab_id, meta in sorted(pm.get("skab", {}).items()):
        skab_truth(skab_id, meta)

    print("\n=== T01 summary ===")
    print(json.dumps(t01["values"], indent=2, default=_json_default))
    print("takeoff/landing events:", [e for e in t01["events"] if e["kind"] in ("takeoff", "landing")])
    print("\n=== T02 summary ===")
    print("num mode changes:", t02["values"]["num_mode_changes"])
    print(t02["notes"])


if __name__ == "__main__":
    main()
