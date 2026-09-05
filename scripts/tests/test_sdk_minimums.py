import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from release_tools import validate_sdk_minimum
from submit_to_registry import build_registry_entry


class SdkMinimumTests(unittest.TestCase):
    def test_minimum_is_independent_of_build_sdk(self):
        manifest = {"min_sdk_required": "0.28.0"}
        self.assertEqual(validate_sdk_minimum(manifest, "0.28.0"), [])
        self.assertEqual(validate_sdk_minimum(manifest, "0.29.0"), [])
        self.assertTrue(validate_sdk_minimum(manifest, "0.27.0"))
        self.assertEqual(manifest["min_sdk_required"], "0.28.0")
        self.assertEqual(validate_sdk_minimum({"min_sdk_required": "0.9.0"}, "0.10.0"), [])
        self.assertTrue(validate_sdk_minimum({"min_sdk_required": "0.10.0"}, "0.9.0"))

    def test_missing_or_invalid_stable_versions_are_rejected(self):
        self.assertTrue(validate_sdk_minimum({}, "0.28.0"))
        for value in (None, 28, "", "0.28", "^0.28.0", "00.28.0", "0.28.0-rc.1", "0.28.0\n"):
            with self.subTest(value=value):
                self.assertTrue(validate_sdk_minimum({"min_sdk_required": value}, "0.28.0"))
                self.assertTrue(validate_sdk_minimum({"min_sdk_required": "0.28.0"}, value))

    def test_cli_checks_manifests_and_bump_preserves_minimums(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            scripts = root / "scripts"
            scripts.mkdir()
            for name in ("release_tools.py", "bump_core_version.py"):
                shutil.copy(SCRIPTS / name, scripts / name)
            (root / "SDK_VERSION").write_text("0.28.0\n")
            manifest_path = root / "example" / "manifest.json"
            manifest_path.parent.mkdir()
            manifest = {
                "id": "example", "name": "Example", "version": "1.0.0",
                "category": "message_parser", "min_sdk_required": "0.28.0",
            }
            manifest_path.write_text(json.dumps(manifest))
            original = manifest_path.read_bytes()

            def run(script, *args):
                return subprocess.run(
                    [sys.executable, str(scripts / script), *args],
                    cwd=root, capture_output=True, text=True, check=False,
                )

            self.assertEqual(run("bump_core_version.py", "--check").returncode, 0)
            self.assertEqual(run("bump_core_version.py", "0.29.0").returncode, 0)
            self.assertEqual((root / "SDK_VERSION").read_text(), "0.29.0\n")
            self.assertEqual(manifest_path.read_bytes(), original)
            self.assertEqual(run("bump_core_version.py", "--check").returncode, 0)
            for minimum in ("0.30.0", None):
                manifest["min_sdk_required"] = minimum
                manifest_path.write_text(json.dumps(manifest))
                result = run("bump_core_version.py", "--check")
                self.assertEqual(result.returncode, 1)
                self.assertIn("example/manifest.json", result.stderr)
                result = run("release_tools.py", "validate-manifest", str(manifest_path))
                self.assertEqual(result.returncode, 1)
                self.assertIn("min_sdk_required", result.stderr)

    def test_registry_preserves_declared_minimum(self):
        manifest = {"id": "example", "min_sdk_required": "0.28.0"}
        self.assertEqual(build_registry_entry(manifest, {})["min_sdk_required"], "0.28.0")


if __name__ == "__main__":
    unittest.main()
