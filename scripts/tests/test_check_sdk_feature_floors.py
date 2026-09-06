import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_sdk_feature_floors as checker
from feature_floor_check import validate_manifest_shape

REPO_ROOT = SCRIPTS.parent

TABLE = {
    "schema_version": 1,
    "minimum_supported_floor": "0.28.0",
    "since": {
        "0.32.0": {"newCall": "vtable::new_call"},
        "0.30.0": {"hardContract": {"declaration": "wire contract", "negotiated": False}},
    },
    "baseline": ["oldCall"],
}


class FixtureRepo:
    """A throwaway repo with one plugin and a surface table."""

    def __init__(self, tmp: Path):
        self.root = tmp
        (tmp / "scripts").mkdir()
        self.table_path = tmp / "scripts" / checker.INTERIM_TABLE_NAME
        self.write_table(TABLE)

    def write_table(self, table: dict, name: str | None = None) -> Path:
        path = self.table_path if name is None else self.root / name
        path.write_text(json.dumps(table))
        return path

    def write_plugin(
        self,
        *,
        name: str = "example_plugin",
        source: str = "int plugin_entry() { return 0; }\n",
        min_sdk_required: str = "0.28.0",
        suggested: str | None = None,
        floor_test: str | None = None,
        links: tuple[str, ...] = (),
        test_source: str | None = None,
    ) -> Path:
        plugin_dir = self.root / name
        plugin_dir.mkdir()
        manifest: dict = {"id": name, "min_sdk_required": min_sdk_required}
        if suggested is not None:
            manifest["suggested_sdk_version"] = suggested
        if floor_test is not None:
            manifest["floor_test"] = floor_test
        (plugin_dir / "manifest.json").write_text(json.dumps(manifest))
        (plugin_dir / "plugin.cpp").write_text(source)
        link_line = ""
        if links:
            link_line = f"target_link_libraries({name} PRIVATE " + " ".join(links) + ")\n"
        (plugin_dir / "CMakeLists.txt").write_text(
            f"add_library({name} SHARED plugin.cpp)\n"
            + link_line
            + f"pj_emit_plugin_manifest({name} MANIFEST_FILE manifest.json)\n"
        )
        if test_source is not None:
            (plugin_dir / "tests").mkdir()
            (plugin_dir / "tests" / "plugin_test.cpp").write_text(test_source)
        return plugin_dir

    def write_common(self, name: str, source: str) -> None:
        common_dir = self.root / "common" / name
        common_dir.mkdir(parents=True)
        (common_dir / f"{name}.cpp").write_text(source)
        (common_dir / "CMakeLists.txt").write_text(f"add_library({name} STATIC {name}.cpp)\n")

    def run(self) -> tuple[bool, str, str]:
        out = io.StringIO()
        err = io.StringIO()
        ok = checker.check_sdk_feature_floors(self.root, out=out, err=err)
        return ok, out.getvalue(), err.getvalue()


class VerdictTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = FixtureRepo(Path(self._tmp.name))
        self.addCleanup(self._tmp.cleanup)

    def test_floor_covering_all_matches_passes(self):
        self.repo.write_plugin(source="void f() { oldCall(); }\n")
        ok, out, _ = self.repo.run()
        self.assertTrue(ok)
        self.assertIn("PASS example_plugin", out)

    def test_newer_surface_without_declared_degradation_fails(self):
        self.repo.write_plugin(source="void f() { newCall(); }\n")
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("newCall (since 0.32.0, at example_plugin/plugin.cpp:1) exceeds floor 0.28.0", err)
        self.assertIn("suggested_sdk_version is missing", err)

    def test_raised_floor_covers_newer_surface(self):
        self.repo.write_plugin(source="void f() { newCall(); }\n", min_sdk_required="0.32.0")
        ok, _, _ = self.repo.run()
        self.assertTrue(ok)

    def test_commented_identifier_does_not_match(self):
        self.repo.write_plugin(source="// newCall in prose only\nint x;\n")
        ok, _, _ = self.repo.run()
        self.assertTrue(ok)

    def test_substring_does_not_match(self):
        self.repo.write_plugin(source="void mynewCallish();\n")
        ok, _, _ = self.repo.run()
        self.assertTrue(ok)

    def test_floor_below_supported_minimum_fails(self):
        self.repo.write_plugin(min_sdk_required="0.27.0")
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("below the supported floor", err)

    def test_overdeclared_floor_only_warns(self):
        self.repo.write_plugin(source="void f() { oldCall(); }\n", min_sdk_required="0.31.0")
        ok, _, err = self.repo.run()
        self.assertTrue(ok)
        self.assertIn("WARN example_plugin", err)


class FloorContractTests(unittest.TestCase):
    GOOD_TEST = "TEST(Suite, WorksAtFloor) { }\n"

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = FixtureRepo(Path(self._tmp.name))
        self.addCleanup(self._tmp.cleanup)

    def test_declared_degradation_passes(self):
        self.repo.write_plugin(
            source="void f() { newCall(); }\n",
            suggested="0.32.0",
            floor_test="Suite.WorksAtFloor",
            test_source=self.GOOD_TEST,
        )
        ok, out, _ = self.repo.run()
        self.assertTrue(ok)
        self.assertIn("full features at 0.32.0", out)

    def test_surface_above_floor_requires_suggested(self):
        self.repo.write_plugin(source="void f() { newCall(); }\n")
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("suggested_sdk_version is missing", err)
        self.assertIn("declare suggested_sdk_version 0.32.0", err)

    def test_wrong_suggested_value_fails(self):
        self.repo.write_plugin(
            source="void f() { newCall(); }\n",
            suggested="0.30.0",
            floor_test="Suite.WorksAtFloor",
            test_source=self.GOOD_TEST,
        )
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("full-feature floor over the matched surfaces is 0.32.0", err)

    def test_suggested_without_gap_is_forbidden(self):
        self.repo.write_plugin(
            source="void f() { oldCall(); }\n",
            suggested="0.32.0",
            floor_test="Suite.WorksAtFloor",
            test_source=self.GOOD_TEST,
        )
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("remove it", err)

    def test_suggested_without_floor_test_fails_shape(self):
        self.repo.write_plugin(source="void f() { newCall(); }\n", suggested="0.32.0")
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("floor_test is required alongside suggested_sdk_version", err)

    def test_floor_test_without_suggested_fails_shape(self):
        self.repo.write_plugin(floor_test="Suite.WorksAtFloor", test_source=self.GOOD_TEST)
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("only meaningful alongside suggested_sdk_version", err)

    def test_missing_floor_test_gtest_fails(self):
        self.repo.write_plugin(
            source="void f() { newCall(); }\n",
            suggested="0.32.0",
            floor_test="Suite.DoesNotExist",
            test_source=self.GOOD_TEST,
        )
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("not found in the plugin's tests", err)

    def test_commented_out_floor_test_fails(self):
        self.repo.write_plugin(
            source="void f() { newCall(); }\n",
            suggested="0.32.0",
            floor_test="Suite.WorksAtFloor",
            test_source="// TEST(Suite, WorksAtFloor) { }\n",
        )
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("not found in the plugin's tests", err)

    def test_non_negotiated_surface_above_floor_always_fails(self):
        # Declaring the degraded range cannot waive a hard protocol surface.
        self.repo.write_plugin(
            source="void f() { hardContract(); newCall(); }\n",
            suggested="0.32.0",
            floor_test="Suite.WorksAtFloor",
            test_source=self.GOOD_TEST,
        )
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("is not\nruntime-negotiated — raise min_sdk_required".replace("\n", " "), err)

    def test_legacy_exceptions_block_is_rejected(self):
        self.repo.write_plugin(
            source="void f() { oldCall(); }\n",
        )
        manifest_path = self.repo.root / "example_plugin" / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["sdk_floor_exceptions"] = {"x": {"reason": "r", "test": "S.N"}}
        manifest_path.write_text(json.dumps(manifest))
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("no longer supported", err)

    def test_shape_validator_branches(self):
        self.assertEqual(validate_manifest_shape({}), [])
        self.assertTrue(validate_manifest_shape({"sdk_floor_exceptions": {}}))  # any presence rejected
        self.assertTrue(validate_manifest_shape({"suggested_sdk_version": "nope"}))
        self.assertTrue(
            validate_manifest_shape(
                {"min_sdk_required": "0.30.0", "suggested_sdk_version": "0.29.0", "floor_test": "S.N"}
            )
        )
        self.assertTrue(validate_manifest_shape({"suggested_sdk_version": "0.32.0"}))
        self.assertTrue(validate_manifest_shape({"floor_test": "S.N"}))
        self.assertTrue(
            validate_manifest_shape({"suggested_sdk_version": "0.32.0", "floor_test": "nodot"})
        )
        self.assertEqual(
            validate_manifest_shape(
                {"min_sdk_required": "0.28.0", "suggested_sdk_version": "0.32.0", "floor_test": "S.N"}
            ),
            [],
        )


class CutoverTests(unittest.TestCase):
    """SDK_VERSION >= 0.33.0 with interim artifacts left = hard failure."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = FixtureRepo(Path(self._tmp.name))
        self.addCleanup(self._tmp.cleanup)
        self.repo.write_plugin()

    def test_interim_table_after_cutover_fails(self):
        (self.repo.root / "SDK_VERSION").write_text("0.33.0\n")
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("delete the interim artifacts", err)

    def test_interim_table_before_cutover_still_works(self):
        (self.repo.root / "SDK_VERSION").write_text("0.32.0\n")
        ok, _, _ = self.repo.run()
        self.assertTrue(ok)


class TablePrecedenceTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = FixtureRepo(Path(self._tmp.name))
        self.addCleanup(self._tmp.cleanup)
        self.repo.write_plugin(source="void f() { newCall(); }\n", min_sdk_required="0.32.0")

    def test_sdk_table_preferred_and_matching_interim_noted(self):
        sdk_table = self.repo.write_table(TABLE, name="feature_floors.json")
        out = io.StringIO()
        err = io.StringIO()
        ok = checker.check_sdk_feature_floors(
            self.repo.root, sdk_floors_path=sdk_table, out=out, err=err
        )
        self.assertTrue(ok)
        self.assertIn("delete the interim copy", err.getvalue())

    def test_diverging_interim_copy_fails(self):
        diverged = dict(TABLE, minimum_supported_floor="0.29.0")
        sdk_table = self.repo.write_table(diverged, name="feature_floors.json")
        err = io.StringIO()
        ok = checker.check_sdk_feature_floors(
            self.repo.root, sdk_floors_path=sdk_table, out=io.StringIO(), err=err
        )
        self.assertFalse(ok)
        self.assertIn("differs from the SDK table", err.getvalue())

    def test_interim_fallback_warns(self):
        _, _, err = self.repo.run()
        self.assertIn("interim surface table", err)


class ClosureTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = FixtureRepo(Path(self._tmp.name))
        self.addCleanup(self._tmp.cleanup)

    def test_common_library_usage_attributes_to_plugin(self):
        self.repo.write_common("shared_helper", "void helper() { newCall(); }\n")
        self.repo.write_plugin(links=("shared_helper",))
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("newCall", err)

    def test_unreachable_common_library_is_not_scanned(self):
        self.repo.write_common("shared_helper", "void helper() { newCall(); }\n")
        self.repo.write_plugin()
        ok, _, _ = self.repo.run()
        self.assertTrue(ok)

    def test_unresolved_link_expression_fails_closed(self):
        self.repo.write_plugin(links=("${MYSTERY_LIBS}",))
        ok, _, err = self.repo.run()
        self.assertFalse(ok)
        self.assertIn("unresolved link items", err)


class RealRepoAcceptanceTests(unittest.TestCase):
    """The audit gap the checker exists to catch, against the REAL tree."""

    def test_lowered_mosaico_floor_fails_on_complete_ingest(self):
        out = io.StringIO()
        err = io.StringIO()
        real_manifest = json.loads((REPO_ROOT / "toolbox_mosaico" / "manifest.json").read_text())
        self.assertGreaterEqual(
            tuple(map(int, real_manifest["min_sdk_required"].split("."))), (0, 30, 0)
        )

        with tempfile.TemporaryDirectory() as tmp:
            # Mirror the repo root with the real scripts/, common/, and
            # toolbox_mosaico sources, but a lowered manifest floor.
            root = Path(tmp)
            (root / "scripts").mkdir()
            (root / "scripts" / checker.INTERIM_TABLE_NAME).write_bytes(
                (SCRIPTS / checker.INTERIM_TABLE_NAME).read_bytes()
            )
            import shutil

            shutil.copytree(REPO_ROOT / "common", root / "common")
            shutil.copytree(REPO_ROOT / "toolbox_mosaico", root / "toolbox_mosaico")
            lowered = dict(real_manifest, min_sdk_required="0.28.0")
            (root / "toolbox_mosaico" / "manifest.json").write_text(json.dumps(lowered))

            ok = checker.check_sdk_feature_floors(root, out=out, err=err)
        self.assertFalse(ok)
        # Without a declared degraded range the lowered floor must fail: a
        # 0.30 surface is matched, and no suggested_sdk_version covers it.
        self.assertIn("exceeds floor 0.28.0", err.getvalue())
        self.assertIn("suggested_sdk_version is missing", err.getvalue())
        self.assertIn("since 0.30.0", err.getvalue())


if __name__ == "__main__":
    unittest.main()
