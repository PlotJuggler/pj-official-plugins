import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def resolve(tag, os_label, arch, cwd=ROOT):
    result = subprocess.run(
        [
            sys.executable, str(ROOT / "scripts" / "release_tools.py"), "resolve-build-scope",
            "--tag", tag, "--tag-type", "tag", "--os-label", os_label, "--arch", arch,
        ],
        cwd=cwd, capture_output=True, text=True, check=True,
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


class QtLinkedPluginBuildScopeTests(unittest.TestCase):
    """A non-widget plugin on the QT_LINKED_PLUGINS list asks for Qt like a widget does.

    The plugin directory is a fixture: resolve-build-scope reads <plugin>/manifest.json relative to
    the working directory, so the test runs it from a scratch tree.
    """

    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.tree = Path(scratch.name)
        plugin = self.tree / "data_stream_can"
        plugin.mkdir()
        (plugin / "conanfile.py").write_text("# fixture\n")
        (plugin / "manifest.json").write_text(
            json.dumps({"category": "data_stream", "supported_platforms": ["linux-x86_64", "windows-x86_64"]})
        )

    def test_requests_qt_only_where_the_official_qt_is_installed(self):
        for os_label, arch, built in (("linux", "x86_64", True), ("windows", "x64", True), ("macos", "arm64", False)):
            with self.subTest(leg=f"{os_label}-{arch}"):
                scope = resolve("data_stream_can/v0.1.0", os_label, arch, cwd=self.tree)
                self.assertEqual(scope["skip"], "false" if built else "true")
                self.assertEqual(scope["needs_qt"], "true" if built else "false")


if __name__ == "__main__":
    unittest.main()
