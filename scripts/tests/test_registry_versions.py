import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from release_tools import semver_sort_key
from submit_to_registry import prune_registry, update_registry

LINUX, MACOS, WINDOWS = "linux-x86_64", "macos-arm64", "windows-x86_64"


def record(version, floor=None, platforms=(LINUX, MACOS), ext_id="csv-loader"):
    entry = {"id": ext_id, "version": version, "platforms": {p: {"url": f"{ext_id}-{version}-{p}"} for p in platforms}}
    if floor is not None:
        entry["min_sdk_required"] = floor
    return entry


def served(extensions):
    """The (id, version, platforms) triples a registry holds, in file order."""
    return [(e["id"], e["version"], sorted(e["platforms"])) for e in extensions]


def submit(extensions, entry):
    return served(update_registry({"extensions": extensions}, entry)["extensions"])


class SemverPrecedenceTests(unittest.TestCase):
    def test_orders_by_semver_precedence(self):
        ordered = [
            "0.9.10",
            "1.0.0-alpha",
            "1.0.0-alpha.1",
            "1.0.0-alpha.beta",
            "1.0.0-dev.2",
            "1.0.0-dev.10",
            "1.0.0-rc.1",
            "1.0.0",
            "1.0.1",
            "1.2.0-pj4.rc.1",
            "1.2.0",
            "1.10.0",
        ]
        self.assertEqual(sorted(reversed(ordered), key=semver_sort_key), ordered)

    def test_build_metadata_does_not_change_precedence(self):
        self.assertEqual(semver_sort_key("1.0.0+linux"), semver_sort_key("1.0.0"))


class PruneRuleTests(unittest.TestCase):
    def test_same_floor_keeps_only_the_newest(self):
        registry = [record("1.0.0", "0.28.0"), record("1.1.0", "0.28.0"), record("1.2.0", "0.28.0")]
        self.assertEqual(served(prune_registry(registry)), [("csv-loader", "1.2.0", [LINUX, MACOS])])

    def test_floor_raise_keeps_both_steps(self):
        registry = [record("1.0.0", "0.28.0"), record("1.1.0", "0.28.0"), record("2.0.0", "0.34.0")]
        self.assertEqual(
            served(prune_registry(registry)),
            [("csv-loader", "1.1.0", [LINUX, MACOS]), ("csv-loader", "2.0.0", [LINUX, MACOS])],
        )

    def test_floor_lowered_again_drops_the_higher_floor_in_between(self):
        registry = [record("1.0.0", "0.28.0"), record("1.1.0", "0.34.0"), record("1.2.0", "0.28.0")]
        self.assertEqual(served(prune_registry(registry)), [("csv-loader", "1.2.0", [LINUX, MACOS])])

    def test_newer_version_missing_a_platform_keeps_the_older_one_for_it_only(self):
        registry = [record("1.0.0", "0.31.0"), record("1.1.0", "0.31.0", platforms=(LINUX,))]
        pruned = prune_registry(registry)
        self.assertEqual(served(pruned), [("csv-loader", "1.0.0", [MACOS]), ("csv-loader", "1.1.0", [LINUX])])
        self.assertEqual(pruned[0]["platforms"][MACOS], registry[0]["platforms"][MACOS])
        self.assertEqual(sorted(registry[0]["platforms"]), [LINUX, MACOS], "the input record must not be mutated")

    def test_missing_floor_is_the_lowest_floor(self):
        registry = [record("1.0.0"), record("1.1.0"), record("2.0.0", "0.28.0")]
        self.assertEqual(
            served(prune_registry(registry)),
            [("csv-loader", "1.1.0", [LINUX, MACOS]), ("csv-loader", "2.0.0", [LINUX, MACOS])],
        )

    def test_pre_release_is_older_than_its_release(self):
        registry = [record("1.0.0", "0.28.0"), record("1.0.0-dev.2", "0.28.0"), record("1.0.0-dev.10", "0.28.0")]
        self.assertEqual(served(prune_registry(registry)), [("csv-loader", "1.0.0", [LINUX, MACOS])])

    def test_pre_release_floor_is_lower_than_its_release(self):
        registry = [record("1.0.0", "0.34.0-dev.1"), record("1.1.0", "0.34.0")]
        self.assertEqual(len(prune_registry(registry)), 2)

    def test_ids_are_pruned_independently_and_sorted_by_id_then_version(self):
        registry = [
            record("2.0.0", "0.34.0", ext_id="b"),
            record("1.0.0", "0.28.0", ext_id="b"),
            record("1.10.0", "0.34.0", ext_id="a"),
            record("1.9.0", "0.28.0", ext_id="a"),
        ]
        self.assertEqual([(i, v) for i, v, _ in served(prune_registry(registry))],
                         [("a", "1.9.0"), ("a", "1.10.0"), ("b", "1.0.0"), ("b", "2.0.0")])


class UpdateRegistryTests(unittest.TestCase):
    def test_new_version_at_the_same_floor_replaces_the_old_one(self):
        self.assertEqual(submit([record("1.0.0", "0.28.0")], record("1.1.0", "0.28.0")),
                         [("csv-loader", "1.1.0", [LINUX, MACOS])])

    def test_new_version_with_a_higher_floor_keeps_the_old_one(self):
        self.assertEqual(submit([record("1.0.0", "0.28.0")], record("1.1.0", "0.34.0")),
                         [("csv-loader", "1.0.0", [LINUX, MACOS]), ("csv-loader", "1.1.0", [LINUX, MACOS])])

    def test_other_extensions_are_left_alone(self):
        other = record("0.1.0", "0.28.0", ext_id="mcap-loader")
        result = update_registry({"extensions": [other]}, record("1.0.0", "0.28.0"))
        self.assertEqual(result["extensions"], [record("1.0.0", "0.28.0"), other])

    def test_resubmitting_an_existing_version_is_rejected(self):
        for version in ("1.0.0", "1.0.0+rebuild"):
            with self.subTest(version=version), self.assertRaises(SystemExit) as raised:
                update_registry({"extensions": [record("1.0.0", "0.28.0")]}, record(version, "0.28.0"))
            self.assertIn("already", str(raised.exception.code))

    def test_backfilling_an_older_version_keeps_only_what_it_still_serves(self):
        newer = record("1.1.0", "0.31.0", platforms=(LINUX,))
        self.assertEqual(submit([newer], record("1.0.0", "0.31.0")),
                         [("csv-loader", "1.0.0", [MACOS]), ("csv-loader", "1.1.0", [LINUX])])

    def test_submitting_a_version_that_serves_no_host_is_rejected(self):
        with self.assertRaises(SystemExit) as raised:
            update_registry({"extensions": [record("1.1.0", "0.28.0")]}, record("1.0.0", "0.28.0"))
        self.assertIn("1.1.0", str(raised.exception.code))


if __name__ == "__main__":
    unittest.main()
