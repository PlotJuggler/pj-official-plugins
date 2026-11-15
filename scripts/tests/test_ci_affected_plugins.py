import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ci_affected_plugins import affected_plugins, build_plan


class AffectedPluginsTest(unittest.TestCase):
    def test_markdown_only_selects_nothing(self):
        self.assertEqual(affected_plugins(["README.md", "data_load_csv/CHANGELOG.md"]), (False, []))

    def test_plugin_change_selects_only_that_plugin(self):
        self.assertEqual(
            affected_plugins(["data_load_csv/src/csv_parser.cpp", "data_load_csv/manifest.json"]),
            (False, ["data_load_csv"]),
        )

    def test_common_library_selects_its_link_consumers(self):
        all_, plugins = affected_plugins(["common/arrow_helpers/src/x.cpp"])
        self.assertFalse(all_)
        self.assertIn("data_load_parquet", plugins)
        self.assertNotIn("data_load_csv", plugins)

    def test_common_library_linked_only_by_tests_selects_those_plugins(self):
        all_, plugins = affected_plugins(["common/test_support/include/x.hpp"])
        self.assertFalse(all_)
        self.assertIn("toolbox_transform_editor", plugins)

    def test_plugins_outside_the_lane_are_dropped(self):
        self.assertEqual(affected_plugins(["widget_basic/src/a.cpp", "data_stream_ros2/a.cpp"]), (False, []))

    def test_global_or_unknown_paths_select_everything(self):
        for path in ("conanfile.py", "SDK_VERSION", "cmake/x.cmake", "common/arrow_timestamp.hpp", "tools/a/b.cpp"):
            with self.subTest(path=path):
                all_, plugins = affected_plugins(["data_load_csv/a.cpp", path])
                self.assertTrue(all_)
                self.assertIn("data_load_mcap", plugins)
                self.assertNotIn("widget_basic", plugins)


class BuildPlanTest(unittest.TestCase):
    def test_one_plugin_builds_standalone_and_splits_arrow(self):
        plan = build_plan(["data_load_parquet/a.cpp", "data_load_csv/a.cpp"])
        self.assertEqual(plan["aggregate"], "false")
        self.assertEqual(plan["standalone"], "data_load_csv data_load_parquet")
        self.assertEqual(plan["plugins"], '["data_load_csv"]')
        self.assertEqual(plan["arrow_plugins"], '["data_load_parquet"]')

    def test_nothing_affected_builds_nothing(self):
        plan = build_plan(["README.md"])
        self.assertEqual((plan["aggregate"], plan["standalone"], plan["plugins"]), ("false", "", "[]"))

    def test_everything_builds_the_aggregate(self):
        plan = build_plan(None)
        self.assertEqual(plan["aggregate"], "true")
        self.assertIn("data_load_mcap", plan["plugins"])

    def test_a_broad_change_switches_to_the_aggregate(self):
        lane = affected_plugins(None)[1]
        self.assertEqual(build_plan([f"{p}/a.cpp" for p in lane[: len(lane) // 2]])["aggregate"], "true")


if __name__ == "__main__":
    unittest.main()
