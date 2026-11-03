import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def resolve(tag, os_label, arch):
    result = subprocess.run(
        [
            sys.executable, str(ROOT / "scripts" / "release_tools.py"), "resolve-build-scope",
            "--tag", tag, "--tag-type", "tag", "--os-label", os_label, "--arch", arch,
        ],
        cwd=ROOT, capture_output=True, text=True, check=True,
    )
    return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)


class WidgetBuildScopeTests(unittest.TestCase):
    def test_widget_library_builds_only_where_the_official_qt_is_installed(self):
        for os_label, arch, built in (
            ("linux", "x86_64", True),
            ("windows", "x64", True),
            ("linux", "aarch64", False),
            ("macos", "arm64", False),
            ("windows", "arm64", False),
        ):
            with self.subTest(leg=f"{os_label}-{arch}"):
                scope = resolve("widget_basic/v1.0.0", os_label, arch)
                self.assertEqual(scope["skip"], "false" if built else "true")
                self.assertEqual(scope["needs_qt"], "true" if built else "false")

    def test_other_plugins_never_request_qt(self):
        for os_label, arch in (("linux", "x86_64"), ("windows", "x64"), ("macos", "arm64")):
            with self.subTest(leg=f"{os_label}-{arch}"):
                self.assertEqual(resolve("toolbox_fft/v1.0.0", os_label, arch)["needs_qt"], "false")


if __name__ == "__main__":
    unittest.main()
