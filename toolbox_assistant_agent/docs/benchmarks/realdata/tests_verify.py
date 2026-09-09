#!/usr/bin/env python3
"""Self-tests for verify.py. Run with: python3 tests_verify.py

Builds synthetic cell directories (fake stream.jsonl / metrics.json /
outcome.json / stdin_sent.txt) and synthetic truth files in a temp directory,
then asserts BOTH directions of every check: a correct reply passes, a wrong
one (time off by 3x tolerance, wrong kind, missing block, invented source,
marker covering the whole flight, ...) fails with the expected reason.

Python 3 stdlib only.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify  # noqa: E402


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------


def assistant_text(text):
    return {"type": "assistant", "message": {"content": [{"type": "text", "text": text}]}}


def tool_use(tool_use_id, name, input_=None):
    return {
        "type": "assistant",
        "message": {"content": [{"type": "tool_use", "id": tool_use_id, "name": name, "input": input_ or {}}]},
    }


def tool_result(tool_use_id, result_obj):
    text = result_obj if isinstance(result_obj, str) else json.dumps(result_obj)
    return {
        "type": "user",
        "message": {"content": [{"tool_use_id": tool_use_id, "type": "tool_result", "content": [{"type": "text", "text": text}]}]},
    }


def write_stream(turn_dir, records):
    lines = "".join(json.dumps(r) + "\n" for r in records)
    (turn_dir / "stream.jsonl").write_text(lines, encoding="utf-8")


def write_stdin_sent(turn_dir, catalog_body, user_text="T01"):
    (turn_dir / "stdin_sent.txt").write_text(catalog_body + "\n\n" + user_text, encoding="utf-8")


def write_metrics(turn_dir, tool_use_counts=None):
    (turn_dir / "metrics.json").write_text(json.dumps({"tool_use_counts": tool_use_counts or {}}), encoding="utf-8")


def write_outcome(turn_dir, created_ids=None, series=None, tabs=None):
    created_ids = created_ids or []
    tabs = tabs or []
    outcome = {
        "created_ids": created_ids,
        "series": series or {},
        "tab_ids": tabs,
        "list_created": {"ok": True, "text": json.dumps({"created": created_ids, "count": len(created_ids)})},
        "plot_tab_list": {"ok": True, "text": json.dumps({"count": len(tabs), "tabs": [{"tab": t} for t in tabs]})},
        "report_status": {"ok": True, "text": "{}"},
    }
    (turn_dir / "outcome.json").write_text(json.dumps(outcome), encoding="utf-8")


def write_cleanup(turn_dir):
    (turn_dir / "cleanup.json").write_text(json.dumps({"remove_markers": {"ok": True, "text": "{}"}}), encoding="utf-8")


def make_cell(tmp_root, cell_name, n_turns=1):
    cell_dir = Path(tmp_root) / cell_name
    for i in range(1, n_turns + 1):
        (cell_dir / f"turn{i}").mkdir(parents=True, exist_ok=True)
    return cell_dir


def reply_with_block(block, prose="Aquí tienes el análisis.\n\n"):
    return prose + "```json\n" + json.dumps(block) + "\n```"


CATALOG_PX4 = (
    'Loaded data (3 topic(s)):\n'
    'dataset "px4":\n'
    '  /fmu/out/vehicle_gps_position: lat (int32), lon (int32), alt (float32)\n'
    '  /fmu/out/vehicle_local_position: z (float32), vx (float32)\n'
    '  /fmu/out/vehicle_land_detected: landed (bool)\n'
)

CATALOG_ALFA = (
    'Loaded data (2 topic(s)):\n'
    'dataset "alfa":\n'
    '  /mavros/rc/out: channel1 (float32), channel2 (float32)\n'
    '  /mavros/imu/data: x (float32), y (float32), z (float32)\n'
)

CATALOG_TRUNCATED = (
    'Loaded data (500 topic(s), listing the first 2):\n'
    'dataset "alfa":\n'
    '  /mavros/imu/data: x (float32)\n'
    '  /mavros/battery: voltage (float32)\n'
    'This listing is TRUNCATED; topics not shown above still exist. Use list_topics with a filter to find them.\n'
)


# ---------------------------------------------------------------------------
# Axis conversion + calibration
# ---------------------------------------------------------------------------


class TestAxisConversion(unittest.TestCase):
    def test_seconds_axis_identity_scale(self):
        ta = {"kind": "ros_epoch", "first_sample_abs": 1000.0}
        self.assertAlmostEqual(verify.truth_to_display_s(1005.0, ta), 5.0)

    def test_csv_datetime_identity_scale(self):
        ta = {"kind": "csv_datetime_utc", "first_sample_abs": 50.0}
        self.assertAlmostEqual(verify.truth_to_display_s(53.25, ta), 3.25)

    def test_ulog_microseconds_scale(self):
        ta = {"unit": "us", "kind": "ulog_us_since_boot", "first_sample_abs": 2_000_000}
        self.assertAlmostEqual(verify.truth_to_display_s(3_000_000, ta), 1.0)
        self.assertAlmostEqual(verify.truth_to_display_s(2_500_000, ta), 0.5)

    def test_calibration_offset_applied_after_scaling(self):
        ta = {"unit": "us", "kind": "ulog_us_since_boot", "first_sample_abs": 0}
        self.assertAlmostEqual(verify.truth_to_display_s(1_000_000, ta, offset_s=0.25), 1.25)

    def test_calibration_file_loaded_by_file_id(self):
        with tempfile.TemporaryDirectory() as d:
            truth_dir = Path(d)
            (truth_dir / "axis_calibration.json").write_text(json.dumps({"alfa_03": {"offset_s": 0.5}}), encoding="utf-8")
            self.assertEqual(verify.load_calibration_offset(truth_dir, "alfa_03"), 0.5)
            self.assertEqual(verify.load_calibration_offset(truth_dir, "alfa_99"), 0.0)  # unlisted file -> default

    def test_calibration_missing_file_defaults_zero(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(verify.load_calibration_offset(Path(d), "anything"), 0.0)


# ---------------------------------------------------------------------------
# Cell directory naming
# ---------------------------------------------------------------------------


class TestCellDirParsing(unittest.TestCase):
    def test_single_task_cell_name(self):
        p = verify.parse_cell_dir(Path("/x/px4-sample_log_small-T01-sonnet-catalog6000"))
        self.assertEqual(p, {"dataset": "px4", "file": "sample_log_small", "model": "sonnet", "arm": "catalog6000", "turns": ["T01"]})

    def test_multi_turn_cell_name(self):
        p = verify.parse_cell_dir(Path("/x/alfa-alfa_03-T07+T08-opus-catalogfull"))
        self.assertEqual(p["turns"], ["T07", "T08"])
        self.assertEqual(p["file"], "alfa_03")
        self.assertEqual(p["model"], "opus")
        self.assertEqual(p["arm"], "catalogfull")

    def test_cell_json_overrides_name(self):
        with tempfile.TemporaryDirectory() as d:
            cell_dir = Path(d) / "weird-name-that-does-not-match"
            cell_dir.mkdir()
            (cell_dir / "cell.json").write_text(
                json.dumps({"dataset": "skab", "file": "skab_02", "model": "haiku", "arm": "catalog6000", "turns": ["T09"]}),
                encoding="utf-8",
            )
            p = verify.parse_cell_dir(cell_dir)
            self.assertEqual(p, {"dataset": "skab", "file": "skab_02", "model": "haiku", "arm": "catalog6000", "turns": ["T09"]})


# ---------------------------------------------------------------------------
# structured_block extraction
# ---------------------------------------------------------------------------


class TestStructuredBlock(unittest.TestCase):
    def test_last_fenced_json_block_wins(self):
        text = (
            "Draft:\n```json\n{\"events\":[],\"values\":{},\"assumptions\":[]}\n```\n"
            "Final:\n```json\n{\"events\":[],\"values\":{},\"assumptions\":[\"final\"]}\n```"
        )
        block, how = verify.extract_json_block(text)
        self.assertEqual(how, "fenced")
        self.assertEqual(block["assumptions"], ["final"])

    def test_bare_json_fallback_when_no_fence(self):
        text = 'Resultado: {"events":[],"values":{},"assumptions":["bare"]} listo.'
        block, how = verify.extract_json_block(text)
        self.assertEqual(how, "bare")
        self.assertEqual(block["assumptions"], ["bare"])

    def test_no_block_found_at_all(self):
        block, how = verify.extract_json_block("no json anywhere in this reply")
        self.assertIsNone(block)
        self.assertIsNone(how)

    def test_reply_text_concatenates_assistant_blocks_in_order(self):
        records = [assistant_text("part one "), tool_use("t1", "mcp__pj__list_topics"), assistant_text("part two")]
        text = verify.assistant_reply_text(records)
        self.assertIn("part one", text)
        self.assertIn("part two", text)
        self.assertLess(text.index("part one"), text.index("part two"))


# ---------------------------------------------------------------------------
# kinds_valid
# ---------------------------------------------------------------------------


class TestKindsValid(unittest.TestCase):
    KINDS = {"T07": ["engine_failure", "control_surface_fault", "no_fault"]}

    def test_allowed_kind_passes(self):
        block = {"events": [{"t_s": 10, "kind": "engine_failure", "detail": "", "source": "/x"}]}
        reason, _ = verify.check_kinds_valid(block, "T07", self.KINDS)
        self.assertEqual(reason, "")

    def test_disallowed_kind_fails(self):
        block = {"events": [{"t_s": 10, "kind": "banana", "detail": "", "source": "/x"}]}
        reason, _ = verify.check_kinds_valid(block, "T07", self.KINDS)
        self.assertNotEqual(reason, "")
        self.assertIn("banana", reason)

    def test_end_suffix_tolerated(self):
        kinds = {"T09": ["anomaly"]}
        block = {"events": [{"t_s": 1, "kind": "anomaly", "source": "/s"}, {"t_s": 2, "kind": "anomaly_end", "source": "/s"}]}
        reason, _ = verify.check_kinds_valid(block, "T09", kinds)
        self.assertEqual(reason, "")

    def test_task_without_kinds_entry_always_passes(self):
        block = {"events": [{"t_s": 1, "kind": "whatever"}]}
        reason, _ = verify.check_kinds_valid(block, "T01", {})
        self.assertEqual(reason, "")


# ---------------------------------------------------------------------------
# no_invented_sources
# ---------------------------------------------------------------------------


class TestNoInventedSources(unittest.TestCase):
    def test_real_source_passes(self):
        block = {"events": [], "values": {"max_altitude_m": {"value": 1, "unit": "m", "source": "/fmu/out/vehicle_gps_position/alt"}}}
        reason, _ = verify.check_no_invented_sources(block, CATALOG_PX4)
        self.assertEqual(reason, "")

    def test_invented_source_fails(self):
        block = {"events": [], "values": {"max_altitude_m": {"value": 1, "unit": "m", "source": "/totally/made/up/topic"}}}
        reason, detail = verify.check_no_invented_sources(block, CATALOG_PX4)
        self.assertNotEqual(reason, "")
        self.assertIn("/totally/made/up/topic", detail["bad_sources"])

    def test_truncated_catalog_relaxes_to_first_segment(self):
        # /mavros is a real first segment in the (truncated) catalog even though
        # this exact sub-topic isn't listed.
        block = {"events": [{"t_s": 1, "kind": "anomaly", "source": "/mavros/some/unlisted/subtopic"}]}
        reason, _ = verify.check_no_invented_sources(block, CATALOG_TRUNCATED)
        self.assertEqual(reason, "")

    def test_truncated_catalog_still_rejects_unrelated_first_segment(self):
        block = {"events": [{"t_s": 1, "kind": "anomaly", "source": "/nonexistent_root/x"}]}
        reason, _ = verify.check_no_invented_sources(block, CATALOG_TRUNCATED)
        self.assertNotEqual(reason, "")


# ---------------------------------------------------------------------------
# T01: values_within_tolerance, source_named, assumptions_present
# ---------------------------------------------------------------------------

TRUTH_T01 = {
    "task": "T01",
    "file": "sample_log_small",
    "time_axis": {"unit": "us", "kind": "ulog_us_since_boot", "first_sample_abs": 0, "last_sample_abs": 200_000_000},
    "events": [],
    "values": {
        "takeoff_s": {"candidates": [{"value": 5_000_000, "source": "manual"}], "tolerance": 1.0},
        "landing_s": {"candidates": [{"value": 150_000_000, "source": "manual"}], "tolerance": 1.0},
        "max_altitude_m": {"candidates": [{"value": 52.3, "source": "gps"}], "tolerance": 1.0},
        "max_ground_speed_mps": {"candidates": [{"value": 12.5, "source": "local_position"}], "tolerance": 0.5},
    },
}
T01_VALUE_NAMES = ["takeoff_s", "landing_s", "max_altitude_m", "max_ground_speed_mps"]


class TestT01Values(unittest.TestCase):
    def _good_block(self):
        return {
            "events": [],
            "values": {
                "takeoff_s": {"value": 5.2, "unit": "s", "source": "/fmu/out/vehicle_land_detected"},
                "landing_s": {"value": 149.5, "unit": "s", "source": "/fmu/out/vehicle_land_detected"},
                "max_altitude_m": {"value": 52.0, "unit": "m", "source": "/fmu/out/vehicle_gps_position"},
                "max_ground_speed_mps": {"value": 12.6, "unit": "m/s", "source": "/fmu/out/vehicle_local_position"},
            },
            "assumptions": ["used GPS altitude, not barometric"],
        }

    def test_values_within_tolerance_pass(self):
        block = self._good_block()
        ta = TRUTH_T01["time_axis"]
        reason, _ = verify.check_values_within_tolerance(block, TRUTH_T01, ta, 0.0, T01_VALUE_NAMES)
        self.assertEqual(reason, "")

    def test_value_off_by_3x_tolerance_fails(self):
        block = self._good_block()
        # tolerance is 1.0m; put the model 3x that away from the only candidate (52.3).
        block["values"]["max_altitude_m"]["value"] = 55.3
        ta = TRUTH_T01["time_axis"]
        reason, detail = verify.check_values_within_tolerance(block, TRUTH_T01, ta, 0.0, T01_VALUE_NAMES)
        self.assertNotEqual(reason, "")
        self.assertIn("max_altitude_m", reason)
        self.assertGreaterEqual(detail["max_altitude_m"]["best_err"], 3.0)

    def test_time_value_axis_converted_before_comparing(self):
        block = self._good_block()
        # takeoff truth candidate is 5_000_000 us -> 5.0s display; put the model
        # 3x tolerance away in DISPLAY seconds (tolerance=1.0s).
        block["values"]["takeoff_s"]["value"] = 8.5
        ta = TRUTH_T01["time_axis"]
        reason, _ = verify.check_values_within_tolerance(block, TRUTH_T01, ta, 0.0, T01_VALUE_NAMES)
        self.assertIn("takeoff_s", reason)

    def test_source_named_pass(self):
        block = self._good_block()
        reason, _ = verify.check_source_named(block, T01_VALUE_NAMES)
        self.assertEqual(reason, "")

    def test_source_named_fails_when_empty(self):
        block = self._good_block()
        block["values"]["landing_s"]["source"] = ""
        reason, detail = verify.check_source_named(block, T01_VALUE_NAMES)
        self.assertNotEqual(reason, "")
        self.assertIn("landing_s", detail["missing"])

    def test_assumptions_present_pass(self):
        self.assertEqual(verify.check_assumptions_present(self._good_block()), "")

    def test_assumptions_present_fails_when_empty(self):
        block = self._good_block()
        block["assumptions"] = []
        self.assertNotEqual(verify.check_assumptions_present(block), "")


# ---------------------------------------------------------------------------
# T02: events_match
# ---------------------------------------------------------------------------

TRUTH_T02 = {
    "task": "T02",
    "file": "sample_log_small",
    "time_axis": {"unit": "s", "kind": "ros_epoch", "first_sample_abs": 1000.0, "last_sample_abs": 1300.0},
    "events": [
        {"t_abs": 1100.0, "kind": "mode_change", "detail": "manual->auto", "tolerance_s": 2.0},
        {"t_abs": 1200.0, "kind": "mode_change", "detail": "auto->hold", "tolerance_s": 2.0},
    ],
    "values": {},
}


class TestT02EventsMatch(unittest.TestCase):
    def test_both_events_matched_pass(self):
        block = {"events": [
            {"t_s": 100.5, "kind": "mode_change", "detail": "", "source": "/nav_state"},
            {"t_s": 199.8, "kind": "mode_change", "detail": "", "source": "/nav_state"},
        ]}
        reason, detail = verify.check_events_match_t02(block, TRUTH_T02, TRUTH_T02["time_axis"], 0.0)
        self.assertEqual(reason, "")
        self.assertEqual(detail["matched"], 2)

    def test_event_time_off_by_3x_tolerance_fails(self):
        # tolerance_s=2.0 -> put one event 6s off, dropping matched ratio below 80%.
        block = {"events": [
            {"t_s": 106.0, "kind": "mode_change", "detail": "", "source": "/nav_state"},
            {"t_s": 199.8, "kind": "mode_change", "detail": "", "source": "/nav_state"},
        ]}
        reason, detail = verify.check_events_match_t02(block, TRUTH_T02, TRUTH_T02["time_axis"], 0.0)
        self.assertNotEqual(reason, "")
        self.assertEqual(detail["matched"], 1)

    def test_too_many_spurious_fails(self):
        block = {"events": [
            {"t_s": 100.5, "kind": "mode_change", "source": "/x"},
            {"t_s": 199.8, "kind": "mode_change", "source": "/x"},
            {"t_s": 10.0, "kind": "mode_change", "source": "/x"},
            {"t_s": 20.0, "kind": "mode_change", "source": "/x"},
        ]}
        reason, detail = verify.check_events_match_t02(block, TRUTH_T02, TRUTH_T02["time_axis"], 0.0)
        self.assertNotEqual(reason, "")
        self.assertEqual(len(detail["spurious"]), 2)


# ---------------------------------------------------------------------------
# T03 (open): events_match with the "none" escape hatch
# ---------------------------------------------------------------------------

TRUTH_T03_WITH_SAT = {
    "task": "T03", "file": "sample_log_small",
    "time_axis": {"unit": "us", "kind": "ulog_us_since_boot", "first_sample_abs": 0},
    "events": [{"t_abs": 40_000_000, "t_end_abs": 45_000_000, "kind": "actuator_saturation", "detail": "aileron", "tolerance_s": 2.0}],
    "values": {},
}
TRUTH_T03_NONE = {
    "task": "T03", "file": "sample_log_small",
    "time_axis": {"unit": "us", "kind": "ulog_us_since_boot", "first_sample_abs": 0},
    "events": [],
    "values": {},
}


class TestT03EventsMatch(unittest.TestCase):
    def test_saturation_reported_inside_interval_passes(self):
        block = {"events": [{"t_s": 41.0, "kind": "actuator_saturation", "detail": "", "source": "/actuator_outputs"}]}
        reason, _ = verify.check_events_match_t03(block, TRUTH_T03_WITH_SAT, TRUTH_T03_WITH_SAT["time_axis"], 0.0)
        self.assertEqual(reason, "")

    def test_no_saturation_event_when_truth_has_one_fails(self):
        block = {"events": [{"t_s": 0.0, "kind": "none"}]}
        reason, _ = verify.check_events_match_t03(block, TRUTH_T03_WITH_SAT, TRUTH_T03_WITH_SAT["time_axis"], 0.0)
        self.assertNotEqual(reason, "")

    def test_none_reported_when_truth_has_none_passes(self):
        block = {"events": [{"t_s": 0.0, "kind": "none", "detail": "", "source": ""}]}
        reason, _ = verify.check_events_match_t03(block, TRUTH_T03_NONE, TRUTH_T03_NONE["time_axis"], 0.0)
        self.assertEqual(reason, "")

    def test_spurious_saturation_when_truth_has_none_fails(self):
        block = {"events": [{"t_s": 10.0, "kind": "actuator_saturation", "detail": "", "source": "/x"}]}
        reason, _ = verify.check_events_match_t03(block, TRUTH_T03_NONE, TRUTH_T03_NONE["time_axis"], 0.0)
        self.assertNotEqual(reason, "")


# ---------------------------------------------------------------------------
# T07: fault_time, fault_kind, fault_surface
# ---------------------------------------------------------------------------

TRUTH_T07_FAULT = {
    "task": "T07", "file": "alfa_03",
    "time_axis": {"unit": "s", "kind": "csv_datetime_utc", "first_sample_abs": 500.0},
    "events": [{"t_abs": 560.0, "kind": "control_surface_fault", "detail": "aileron", "tolerance_s": 2.0}],
    "values": {},
    "post_fault_end_abs": 600.0,
}
TRUTH_T07_NO_FAULT = {
    "task": "T07", "file": "alfa_04",
    "time_axis": {"unit": "s", "kind": "csv_datetime_utc", "first_sample_abs": 500.0},
    "events": [{"t_abs": 0, "kind": "no_fault", "detail": "", "tolerance_s": 2.0}],
    "values": {},
}


class TestT07Fault(unittest.TestCase):
    def test_fault_time_and_kind_and_surface_pass(self):
        block = {"events": [{"t_s": 60.5, "kind": "control_surface_fault", "detail": "el alerón se bloqueó", "source": "/mavros/rc/out"}]}
        ta = TRUTH_T07_FAULT["time_axis"]
        t_reason, t_detail = verify.check_fault_time(block, TRUTH_T07_FAULT, ta, 0.0)
        self.assertEqual(t_reason, "")
        k_reason, _ = verify.check_fault_kind(block, TRUTH_T07_FAULT, ta, 0.0)
        self.assertEqual(k_reason, "")
        # "aileron" (truth) vs the Spanish "alerón" won't substring-match; use an
        # English detail to test the passing direction of the optional check.
        block2 = dict(block)
        block2["events"] = [{"t_s": 60.5, "kind": "control_surface_fault", "detail": "aileron stuck", "source": "/x"}]
        _t2_reason, t2_detail = verify.check_fault_time(block2, TRUTH_T07_FAULT, ta, 0.0)
        s_reason = verify.check_fault_surface(block2, TRUTH_T07_FAULT, t2_detail.get("matched_event"))
        self.assertEqual(s_reason, "")

    def test_fault_time_off_by_3x_tolerance_fails(self):
        # tolerance_s=2.0 -> 6s off
        block = {"events": [{"t_s": 66.0, "kind": "control_surface_fault", "detail": "", "source": "/x"}]}
        ta = TRUTH_T07_FAULT["time_axis"]
        reason, _ = verify.check_fault_time(block, TRUTH_T07_FAULT, ta, 0.0)
        self.assertNotEqual(reason, "")

    def test_wrong_kind_fails_fault_kind_but_not_fault_time(self):
        block = {"events": [{"t_s": 60.5, "kind": "engine_failure", "detail": "", "source": "/x"}]}
        ta = TRUTH_T07_FAULT["time_axis"]
        t_reason, _ = verify.check_fault_time(block, TRUTH_T07_FAULT, ta, 0.0)
        self.assertEqual(t_reason, "")  # time still matches
        k_reason, _ = verify.check_fault_kind(block, TRUTH_T07_FAULT, ta, 0.0)
        self.assertNotEqual(k_reason, "")

    def test_no_fault_truth_with_no_fault_model_passes(self):
        block = {"events": [{"t_s": 0.0, "kind": "no_fault", "detail": "", "source": ""}]}
        ta = TRUTH_T07_NO_FAULT["time_axis"]
        reason, _ = verify.check_fault_time(block, TRUTH_T07_NO_FAULT, ta, 0.0)
        self.assertEqual(reason, "")

    def test_no_fault_truth_with_fault_model_fails(self):
        block = {"events": [{"t_s": 10.0, "kind": "engine_failure", "detail": "", "source": "/x"}]}
        ta = TRUTH_T07_NO_FAULT["time_axis"]
        reason, _ = verify.check_fault_time(block, TRUTH_T07_NO_FAULT, ta, 0.0)
        self.assertNotEqual(reason, "")

    def test_surface_mismatch_fails(self):
        block = {"events": [{"t_s": 60.5, "kind": "control_surface_fault", "detail": "rudder stuck", "source": "/x"}]}
        ta = TRUTH_T07_FAULT["time_axis"]
        _reason, detail = verify.check_fault_time(block, TRUTH_T07_FAULT, ta, 0.0)
        reason = verify.check_fault_surface(block, TRUTH_T07_FAULT, detail.get("matched_event"))
        self.assertNotEqual(reason, "")

    def test_surface_ignores_nonzero_calibration_offset_correctly(self):
        # Regression test: check_fault_surface used to re-derive the matched
        # event with offset_s hardcoded to 0.0, which would silently break
        # (or mis-match) whenever the real calibration offset was nonzero.
        offset_s = 3.7
        block = {"events": [{"t_s": 60.5 + offset_s, "kind": "control_surface_fault", "detail": "aileron stuck", "source": "/x"}]}
        ta = TRUTH_T07_FAULT["time_axis"]
        t_reason, t_detail = verify.check_fault_time(block, TRUTH_T07_FAULT, ta, offset_s)
        self.assertEqual(t_reason, "")
        s_reason = verify.check_fault_surface(block, TRUTH_T07_FAULT, t_detail.get("matched_event"))
        self.assertEqual(s_reason, "")


# ---------------------------------------------------------------------------
# T08: marker_covers_fault, series_created, tab_created
# ---------------------------------------------------------------------------


class TestT08Markers(unittest.TestCase):
    def setUp(self):
        self.truth = TRUTH_T07_FAULT  # fault at t_abs=560 -> display 60.0s, post_fault_end_abs=600 -> 100.0s (40s interval)
        self.ta = self.truth["time_axis"]

    def test_marker_covering_the_fault_stretch_passes(self):
        records = [
            tool_use("tu1", "mcp__pj__create_markers", {"series": "/x", "threshold": 1}),
            tool_result("tu1", {"created_markers_on": "/x", "markers_created": 2, "by_kind": {"regions": 1}, "covered_s": 35.0}),
        ]
        reason, detail = verify.check_marker_covers_fault(records, self.truth, self.ta, 0.0)
        self.assertEqual(reason, "")
        self.assertGreaterEqual(detail["iou_proxy"], 0.3)

    def test_marker_covering_the_whole_flight_fails(self):
        # Truth interval is 40s; a marker set covering the whole ~600s flight is
        # the textbook over-marking failure (design note in tools.cpp: coverage
        # must earn the name, not just exist).
        records = [
            tool_use("tu1", "mcp__pj__create_markers", {"series": "/x", "threshold": 1}),
            tool_result("tu1", {"created_markers_on": "/x", "markers_created": 1, "by_kind": {"regions": 1}, "covered_s": 590.0}),
        ]
        reason, detail = verify.check_marker_covers_fault(records, self.truth, self.ta, 0.0)
        self.assertNotEqual(reason, "")
        self.assertFalse(detail["within_3x_budget"])

    def test_no_create_markers_call_fails(self):
        reason, _ = verify.check_marker_covers_fault([], self.truth, self.ta, 0.0)
        self.assertNotEqual(reason, "")
        self.assertIn("never called", reason)

    def test_zero_coverage_fails(self):
        records = [
            tool_use("tu1", "mcp__pj__create_markers", {"series": "/x", "threshold": 1}),
            tool_result("tu1", {"created_markers_on": "/x", "markers_created": 3, "by_kind": {"events": 3}}),  # no regions -> no covered_s
        ]
        reason, _ = verify.check_marker_covers_fault(records, self.truth, self.ta, 0.0)
        self.assertNotEqual(reason, "")

    def test_series_created_passes_with_finite_count(self):
        outcome = {
            "created_ids": ["assistant_markers", "fault_signal"],
            "series": {"fault_signal": {"ok": True, "text": json.dumps({"series": "fault_signal/value", "stats": {"count": 500}})}},
        }
        reason, _ = verify.check_series_created(outcome)
        self.assertEqual(reason, "")

    def test_series_created_fails_when_nothing_but_markers(self):
        outcome = {"created_ids": ["assistant_markers"], "series": {}}
        reason, _ = verify.check_series_created(outcome)
        self.assertNotEqual(reason, "")

    def test_series_created_fails_when_count_zero(self):
        outcome = {
            "created_ids": ["fault_signal"],
            "series": {"fault_signal": {"ok": True, "text": json.dumps({"series": "fault_signal/value", "stats": {"count": 0}})}},
        }
        reason, _ = verify.check_series_created(outcome)
        self.assertNotEqual(reason, "")

    def test_tab_created_passes(self):
        reason, _ = verify.check_tab_created({"tab_ids": ["view"]})
        self.assertEqual(reason, "")

    def test_tab_created_fails_when_none(self):
        reason, _ = verify.check_tab_created({"tab_ids": []})
        self.assertNotEqual(reason, "")


# ---------------------------------------------------------------------------
# T09: interval_match, sensors_named
# ---------------------------------------------------------------------------

TRUTH_T09 = {
    "task": "T09", "file": "skab_01",
    "time_axis": {"unit": "s", "kind": "csv_datetime_utc", "first_sample_abs": 0.0},
    "events": [{"t_abs": 100.0, "t_end_abs": 130.0, "kind": "anomaly", "detail": "", "tolerance_s": 3.0}],
    "values": {},
}


class TestT09Interval(unittest.TestCase):
    def test_two_event_form_overlapping_interval_passes(self):
        block = {"events": [
            {"t_s": 101.0, "kind": "anomaly", "detail": "", "source": "/pressure"},
            {"t_s": 128.0, "kind": "anomaly_end", "detail": "", "source": "/pressure"},
        ]}
        reason, detail = verify.check_interval_match_t09(block, TRUTH_T09, TRUTH_T09["time_axis"], 0.0)
        self.assertEqual(reason, "")
        self.assertGreaterEqual(detail["iou"], 0.3)

    def test_detail_embedded_end_time_form_passes(self):
        block = {"events": [{"t_s": 102.0, "kind": "anomaly", "detail": "anomaly from 102 to 128", "source": "/pressure"}]}
        reason, _ = verify.check_interval_match_t09(block, TRUTH_T09, TRUTH_T09["time_axis"], 0.0)
        self.assertEqual(reason, "")

    def test_far_off_interval_fails(self):
        block = {"events": [
            {"t_s": 400.0, "kind": "anomaly", "detail": "", "source": "/pressure"},
            {"t_s": 420.0, "kind": "anomaly_end", "detail": "", "source": "/pressure"},
        ]}
        reason, detail = verify.check_interval_match_t09(block, TRUTH_T09, TRUTH_T09["time_axis"], 0.0)
        self.assertNotEqual(reason, "")
        self.assertLess(detail["iou"], 0.3)

    def test_sensors_named_passes(self):
        block = {"events": [{"t_s": 101.0, "kind": "anomaly", "source": "/pressure"}]}
        self.assertEqual(verify.check_sensors_named(block), "")

    def test_sensors_named_fails_when_empty_source(self):
        block = {"events": [{"t_s": 101.0, "kind": "anomaly", "source": ""}]}
        self.assertNotEqual(verify.check_sensors_named(block), "")


# ---------------------------------------------------------------------------
# T10 (open): first_sensor (optional-only)
# ---------------------------------------------------------------------------

TRUTH_T10 = {
    "task": "T10", "file": "skab_01",
    "time_axis": {"unit": "s", "kind": "csv_datetime_utc", "first_sample_abs": 0.0},
    "events": [],
    "values": {"first_deviation_order": {"value": ["/pressure", "/temperature", "/flow"]}},
}


class TestT10FirstSensor(unittest.TestCase):
    def test_named_sensor_in_top_two_passes(self):
        block = {"events": [], "values": {"first_sensor": {"value": "/temperature", "unit": "", "source": "/temperature"}}}
        self.assertEqual(verify.check_first_sensor_t10(block, TRUTH_T10), "")

    def test_named_sensor_from_event_source_in_top_two_passes(self):
        block = {"events": [{"t_s": 1, "kind": "anomaly", "source": "/pressure"}], "values": {}}
        self.assertEqual(verify.check_first_sensor_t10(block, TRUTH_T10), "")

    def test_named_sensor_not_in_top_two_fails(self):
        block = {"events": [], "values": {"first_sensor": {"value": "/flow", "unit": "", "source": "/flow"}}}
        self.assertNotEqual(verify.check_first_sensor_t10(block, TRUTH_T10), "")

    def test_no_sensor_named_fails(self):
        block = {"events": [], "values": {}}
        self.assertNotEqual(verify.check_first_sensor_t10(block, TRUTH_T10), "")


# ---------------------------------------------------------------------------
# no_residue
# ---------------------------------------------------------------------------


class TestNoResidue(unittest.TestCase):
    def test_skipped_when_cleanup_did_not_run(self):
        with tempfile.TemporaryDirectory() as d:
            turn_dir = Path(d)
            reason, detail = verify.check_no_residue("T02", turn_dir)
            self.assertEqual(reason, "")
            self.assertIn("skipped", detail)

    def test_passes_when_clean_before_cleanup(self):
        with tempfile.TemporaryDirectory() as d:
            turn_dir = Path(d)
            write_outcome(turn_dir, created_ids=[], tabs=[])
            write_cleanup(turn_dir)
            reason, _ = verify.check_no_residue("T02", turn_dir)
            self.assertEqual(reason, "")

    def test_fails_on_residue_for_a_closed_task(self):
        with tempfile.TemporaryDirectory() as d:
            turn_dir = Path(d)
            write_outcome(turn_dir, created_ids=["assistant_markers", "leftover"], tabs=["view"])
            write_cleanup(turn_dir)
            reason, _ = verify.check_no_residue("T09", turn_dir)
            self.assertNotEqual(reason, "")

    def test_t03_allows_residue_by_design(self):
        with tempfile.TemporaryDirectory() as d:
            turn_dir = Path(d)
            write_outcome(turn_dir, created_ids=["saturation_markers"], tabs=[])
            write_cleanup(turn_dir)
            reason, detail = verify.check_no_residue("T03", turn_dir)
            self.assertEqual(reason, "")
            self.assertTrue(detail.get("residue_allowed"))


# ---------------------------------------------------------------------------
# End-to-end: build_score over synthetic cell directories
# ---------------------------------------------------------------------------


class TestBuildScoreEndToEnd(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.truth_dir = self.tmp / "truth"
        self.truth_dir.mkdir()
        self.runs_dir = self.tmp / "runs"
        self.runs_dir.mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def _write_truth(self, name, doc):
        (self.truth_dir / name).write_text(json.dumps(doc), encoding="utf-8")

    def test_t01_good_cell_passes(self):
        self._write_truth("T01_sample_log_small.json", TRUTH_T01)
        cell = make_cell(self.runs_dir, "px4-sample_log_small-T01-sonnet-catalog6000")
        turn1 = cell / "turn1"
        block = {
            "events": [],
            "values": {
                "takeoff_s": {"value": 5.2, "unit": "s", "source": "/fmu/out/vehicle_land_detected"},
                "landing_s": {"value": 149.5, "unit": "s", "source": "/fmu/out/vehicle_land_detected"},
                "max_altitude_m": {"value": 52.0, "unit": "m", "source": "/fmu/out/vehicle_gps_position"},
                "max_ground_speed_mps": {"value": 12.6, "unit": "m/s", "source": "/fmu/out/vehicle_local_position"},
            },
            "assumptions": ["used GPS altitude"],
        }
        write_stream(turn1, [assistant_text(reply_with_block(block))])
        write_stdin_sent(turn1, CATALOG_PX4, "T01")
        write_metrics(turn1, {"read_series": 2})
        result = verify.build_score(cell, self.truth_dir, task="T01")
        self.assertTrue(result["pass"], result["checks"])
        self.assertEqual(result["assumptions"], ["used GPS altitude"])
        self.assertEqual(result["tool_use_counts"], {"read_series": 2})

    def test_t01_invented_source_cell_fails(self):
        self._write_truth("T01_sample_log_small.json", TRUTH_T01)
        cell = make_cell(self.runs_dir, "px4-sample_log_small-T01-sonnet-catalog6000")
        turn1 = cell / "turn1"
        block = {
            "events": [],
            "values": {
                "takeoff_s": {"value": 5.2, "unit": "s", "source": "/made/up/topic"},
                "landing_s": {"value": 149.5, "unit": "s", "source": "/made/up/topic"},
                "max_altitude_m": {"value": 52.0, "unit": "m", "source": "/made/up/topic"},
                "max_ground_speed_mps": {"value": 12.6, "unit": "m/s", "source": "/made/up/topic"},
            },
            "assumptions": ["x"],
        }
        write_stream(turn1, [assistant_text(reply_with_block(block))])
        write_stdin_sent(turn1, CATALOG_PX4, "T01")
        write_metrics(turn1)
        result = verify.build_score(cell, self.truth_dir, task="T01")
        self.assertFalse(result["pass"])
        self.assertFalse(result["checks"]["no_invented_sources"]["pass"])

    def test_missing_structured_block_skips_every_claim_check(self):
        self._write_truth("T01_sample_log_small.json", TRUTH_T01)
        cell = make_cell(self.runs_dir, "px4-sample_log_small-T01-sonnet-catalog6000")
        turn1 = cell / "turn1"
        write_stream(turn1, [assistant_text("Se me olvidó el bloque JSON, lo siento.")])
        write_stdin_sent(turn1, CATALOG_PX4, "T01")
        write_metrics(turn1)
        result = verify.build_score(cell, self.truth_dir, task="T01")
        self.assertFalse(result["pass"])
        self.assertFalse(result["structured_block"])
        self.assertEqual(set(result["checks"].keys()), {"structured_block"})
        self.assertEqual(result["checks"]["structured_block"]["reason"], "no_structured_block")

    def test_abort_cell_reports_abort_reason(self):
        self._write_truth("T01_sample_log_small.json", TRUTH_T01)
        cell = make_cell(self.runs_dir, "px4-sample_log_small-T01-sonnet-catalog6000")
        turn1 = cell / "turn1"
        (turn1 / "ABORT.txt").write_text("forbidden word found in catalog before calling the model: 'engine_failure'\n", encoding="utf-8")
        # no stream.jsonl at all -- bench_cli.py never called the model
        result = verify.build_score(cell, self.truth_dir, task="T01")
        self.assertFalse(result["pass"])
        self.assertIn("aborted:", result["checks"]["structured_block"]["reason"])

    def test_multi_turn_cell_scores_t08_from_turn2(self):
        self._write_truth("T07_alfa_03.json", TRUTH_T07_FAULT)
        self._write_truth("T08_alfa_03.json", TRUTH_T07_FAULT)
        cell = make_cell(self.runs_dir, "alfa-alfa_03-T07+T08-sonnet-catalog6000", n_turns=2)
        turn1 = cell / "turn1"
        turn2 = cell / "turn2"

        t07_block = {"events": [{"t_s": 60.5, "kind": "control_surface_fault", "detail": "aileron stuck", "source": "/mavros/rc/out"}], "values": {}, "assumptions": ["surface inferred from control input saturation"]}
        write_stream(turn1, [assistant_text(reply_with_block(t07_block))])
        write_stdin_sent(turn1, CATALOG_ALFA, "T07")
        write_metrics(turn1)

        t08_block = {"events": [{"t_s": 60.5, "kind": "control_surface_fault", "detail": "", "source": "/mavros/rc/out"}], "values": {}, "assumptions": []}
        records2 = [
            assistant_text("Voy a marcarlo."),
            tool_use("tu1", "mcp__pj__create_markers", {"series": "/mavros/rc/out"}),
            tool_result("tu1", {"created_markers_on": "/mavros/rc/out", "markers_created": 1, "by_kind": {"regions": 1}, "covered_s": 35.0}),
            assistant_text(reply_with_block(t08_block)),
        ]
        write_stream(turn2, records2)
        write_stdin_sent(turn2, CATALOG_ALFA, "T08")
        write_metrics(turn2)
        write_outcome(turn2, created_ids=["assistant_markers", "fault_flag"], series={"fault_flag": {"ok": True, "text": json.dumps({"series": "fault_flag/value", "stats": {"count": 300}})}}, tabs=["view"])

        result_t07 = verify.build_score(cell, self.truth_dir, task="T07")
        self.assertTrue(result_t07["pass"], result_t07["checks"])

        result_t08 = verify.build_score(cell, self.truth_dir, task="T08")
        self.assertTrue(result_t08["pass"], result_t08["checks"])
        self.assertEqual(set(result_t08["required"]), {"structured_block", "kinds_valid", "no_invented_sources", "marker_covers_fault", "series_created", "tab_created"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
