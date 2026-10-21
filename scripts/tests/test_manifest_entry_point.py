import json
import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

from release_tools import validate_manifest
from submit_to_registry import build_registry_entry

BASE = {"id": "example", "name": "Example", "version": "1.0.0", "category": "toolbox"}


class EntryPointTests(unittest.TestCase):
    def test_string_entry_point_is_accepted(self):
        for value in ("Toolbox menu → Example", ""):
            with self.subTest(value=value):
                self.assertEqual(validate_manifest({**BASE, "entry_point": value}), [])

    def test_non_string_entry_point_is_rejected(self):
        for value in (None, 1, ["Toolbox menu", "Example"], {"menu": "Toolbox"}, True):
            with self.subTest(value=value):
                errors = validate_manifest({**BASE, "entry_point": value})
                self.assertEqual(len(errors), 1)
                self.assertIn("entry_point", errors[0])

    def test_registry_entry_copies_entry_point_when_present(self):
        manifest = {**BASE, "entry_point": "Toolbox menu → Example"}
        self.assertEqual(build_registry_entry(manifest, {})["entry_point"], "Toolbox menu → Example")

    def test_registry_entry_omits_entry_point_when_absent(self):
        self.assertNotIn("entry_point", build_registry_entry(BASE, {}))

    def test_repository_manifests_declare_string_entry_points(self):
        for path in sorted(REPO.glob("*/manifest.json")):
            manifest = json.loads(path.read_text())
            with self.subTest(manifest=path.parent.name):
                self.assertEqual(validate_manifest(manifest), [])


if __name__ == "__main__":
    unittest.main()
