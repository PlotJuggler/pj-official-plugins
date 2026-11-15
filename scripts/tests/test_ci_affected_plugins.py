import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import ci_affected_plugins
from check_sdk_feature_floors import CheckError
from ci_affected_plugins import affected_plugins, build_plan, pr_changed_files


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
        self.assertEqual(affected_plugins(["widget_basic/src/a.cpp", "widget_log/src/a.cpp"]), (False, []))
        # data_stream_ros2 is out of the lane itself, but parser_ros includes one of its headers.
        self.assertEqual(affected_plugins(["data_stream_ros2/a.cpp"]), (False, ["parser_ros"]))

    def test_global_or_unknown_paths_select_everything(self):
        for path in ("conanfile.py", "SDK_VERSION", "cmake/x.cmake", "common/arrow_timestamp.hpp", "tools/a/b.cpp"):
            with self.subTest(path=path):
                all_, plugins = affected_plugins(["data_load_csv/a.cpp", path])
                self.assertTrue(all_)
                self.assertIn("data_load_mcap", plugins)
                self.assertNotIn("widget_basic", plugins)

    def test_plugins_reaching_into_another_plugin_follow_it(self):
        _, plugins = affected_plugins(["toolbox_assistant_agent/tests/support/fake_multi_dataset_host.hpp"])
        self.assertTrue({"toolbox_fft", "toolbox_data_exporter"} <= set(plugins))
        self.assertIn("parser_ros", affected_plugins(["data_stream_ros2/ros2_type_name.hpp"])[1])
        # data_exporter_isolation_test loads build/data_load_parquet/... next to the exporter.
        self.assertIn("toolbox_data_exporter", affected_plugins(["data_load_parquet/src/a.cpp"])[1])

    def test_common_cmake_change_selects_everything(self):
        # Root CMake configures several common/ libraries in EVERY single-plugin build.
        self.assertTrue(affected_plugins(["common/pj_video_demux/CMakeLists.txt"])[0])

    def test_unparsable_cmake_builds_the_plugin_instead_of_crashing(self):
        with mock.patch.object(ci_affected_plugins, "parse_cmake_model", side_effect=CheckError("bad")):
            self.assertIn("data_load_csv", affected_plugins(["common/base64/include/x.hpp"])[1])


class PrChangedFilesTest(unittest.TestCase):
    def test_a_moved_file_lists_both_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            git = ["git", "-C", tmp, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.hooksPath=/dev/null"]
            subprocess.run([*git, "init", "-q"], check=True)
            (Path(tmp) / "common" / "base64").mkdir(parents=True)
            (Path(tmp) / "common" / "base64" / "b.hpp").write_text("int b;\n" * 20)
            subprocess.run([*git, "add", "-A"], check=True)
            subprocess.run([*git, "commit", "-qm", "base"], check=True)
            (Path(tmp) / "parser_protobuf").mkdir()
            subprocess.run([*git, "mv", "common/base64/b.hpp", "parser_protobuf/b.hpp"], check=True)
            subprocess.run([*git, "commit", "-qm", "move"], check=True)
            self.assertEqual(
                sorted(pr_changed_files(Path(tmp))), ["common/base64/b.hpp", "parser_protobuf/b.hpp"]
            )


class BuildPlanTest(unittest.TestCase):
    def test_one_plugin_builds_standalone_and_splits_arrow(self):
        plan = build_plan(["data_load_parquet/a.cpp", "data_load_csv/a.cpp"])
        self.assertEqual(plan["aggregate"], "false")
        # toolbox_data_exporter follows parquet: its isolation test loads the parquet build.
        self.assertEqual(plan["standalone"], "data_load_csv data_load_parquet toolbox_data_exporter")
        self.assertEqual(json.loads(plan["plugins"]), ["data_load_csv"])
        self.assertEqual(json.loads(plan["arrow_plugins"]), ["data_load_parquet", "toolbox_data_exporter"])

    def test_nothing_affected_builds_nothing(self):
        plan = build_plan(["README.md"])
        self.assertEqual(
            {k: plan[k] for k in ("aggregate", "standalone", "windows_aggregate", "windows_standalone")},
            {"aggregate": "false", "standalone": "", "windows_aggregate": "false", "windows_standalone": ""},
        )
        self.assertEqual((json.loads(plan["plugins"]), json.loads(plan["arrow_plugins"])), ([], []))

    def test_everything_builds_the_aggregate(self):
        plan = build_plan(None)
        self.assertEqual((plan["aggregate"], plan["windows_aggregate"]), ("true", "true"))
        self.assertIn("data_load_mcap", json.loads(plan["plugins"]))

    def test_a_recipe_edit_builds_the_windows_aggregate_only(self):
        # Only the aggregate catches a dependency missing from the root recipe or a target
        # name clash across plugins; Windows (sccache) pays for it, macOS stays scoped.
        plan = build_plan(["data_load_csv/conanfile.py"])
        self.assertEqual((plan["aggregate"], plan["standalone"]), ("false", "data_load_csv"))
        self.assertEqual((plan["windows_aggregate"], plan["windows_standalone"]), ("true", ""))
        self.assertEqual(build_plan(["data_load_csv/src/a.cpp"])["windows_aggregate"], "false")

    def test_a_broad_change_switches_to_the_aggregate(self):
        lane = affected_plugins(None)[1]
        self.assertEqual(build_plan([f"{p}/a.cpp" for p in lane[: len(lane) // 2]])["aggregate"], "true")


if __name__ == "__main__":
    unittest.main()
