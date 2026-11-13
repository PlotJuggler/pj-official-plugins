import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

from release_tools import find_previous_release_tag, validate_semver


class StrictSemverTests(unittest.TestCase):
    def test_semver_versions_are_accepted(self):
        for version in ("1.0.0", "0.114.3", "1.0.0-rc.1", "1.0.0-dev.10", "1.2.0-pj4.rc.1", "1.0.0-0a", "1.0.0+build.5"):
            with self.subTest(version=version):
                self.assertTrue(validate_semver(version))

    def test_versions_the_registry_would_reject_are_rejected(self):
        for version in ("1.0", "1.02.0", "01.0.0", "1.0.0-rc.01", "1.0.0-", "1.0.0-a..b", "1.0.0+", "1.0.0\n", "１.0.0"):
            with self.subTest(version=version):
                self.assertFalse(validate_semver(version))


class PreviousReleaseTagTests(unittest.TestCase):
    def test_a_pre_release_is_the_previous_release_of_its_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            def git(*args):
                subprocess.run(["git", "-C", tmp, *args], check=True, capture_output=True)

            git("init", "-q")
            git("-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-q", "--allow-empty", "-m", "x")
            for tag in ("foo/v0.9.0", "foo/v0.10.0", "foo/v1.0.0-rc.2", "foo/v1.0.0-rc.10", "foo/v1.0.0", "bar/v5.0.0"):
                git("tag", tag)

            self.assertEqual(find_previous_release_tag(root, "foo", "foo/v1.0.0"), "foo/v1.0.0-rc.10")
            self.assertEqual(find_previous_release_tag(root, "foo", "foo/v1.0.0-rc.10"), "foo/v1.0.0-rc.2")
            self.assertEqual(find_previous_release_tag(root, "foo", "foo/v1.0.0-rc.2"), "foo/v0.10.0")
            self.assertEqual(find_previous_release_tag(root, "foo", "foo/v0.10.0"), "foo/v0.9.0")
            self.assertIsNone(find_previous_release_tag(root, "foo", "foo/v0.9.0"))


class SubmitVersionTests(unittest.TestCase):
    def test_a_release_that_is_not_the_checked_out_manifest_version_is_refused(self):
        # The registry entry takes its version and min_sdk_required from the manifest on disk.
        result = subprocess.run(
            [sys.executable, str(SCRIPTS / "submit_to_registry.py"), "data_load_csv", "--version", "0.0.1", "--dry-run"],
            cwd=REPO,
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the manifest", result.stderr)
        self.assertNotIn("Registry entry", result.stdout)


if __name__ == "__main__":
    unittest.main()
