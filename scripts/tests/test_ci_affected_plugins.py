import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ci_affected_plugins import affected_plugins


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


if __name__ == "__main__":
    unittest.main()
