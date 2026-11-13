import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from release_tools import semver_sort_key
from submit_to_registry import prune_registry, superseded_entries, update_registry

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

    def test_resubmitting_a_version_with_the_same_platforms_changes_nothing(self):
        extensions = [record("1.0.0", "0.28.0")]
        result = update_registry({"extensions": extensions}, record("1.0.0", "0.28.0"))
        self.assertIs(result["extensions"], extensions)

    def test_resubmitting_a_version_may_add_a_platform(self):
        stored = record("1.0.0", "0.28.0", platforms=(LINUX,))
        stored["description"] = "edited in the registry"
        result = update_registry({"extensions": [stored]}, record("1.0.0", "0.28.0", platforms=(MACOS, LINUX, WINDOWS)))
        (merged,) = result["extensions"]
        self.assertEqual(list(merged["platforms"]), [LINUX, MACOS, WINDOWS], "stored platforms keep their place")
        self.assertEqual(merged["platforms"][LINUX], stored["platforms"][LINUX])
        self.assertEqual(merged["description"], "edited in the registry", "only platforms are added")
        self.assertEqual(list(stored["platforms"]), [LINUX], "the stored record must not be mutated")

    def test_resubmitting_a_version_must_not_change_a_published_artifact(self):
        changed = record("1.0.0", "0.28.0", platforms=(LINUX, MACOS))
        changed["platforms"][LINUX] = {"url": "rebuilt"}
        dropped = record("1.0.0", "0.28.0", platforms=(MACOS,))
        other_build = record("1.0.0+rebuild", "0.28.0", platforms=(LINUX, MACOS))
        for name, entry in (("changed", changed), ("dropped", dropped), ("build", other_build)):
            with self.subTest(name), self.assertRaises(ValueError) as raised:
                update_registry({"extensions": [record("1.0.0", "0.28.0", platforms=(LINUX,))]}, entry)
            self.assertIn("already holds", str(raised.exception))

    def test_resubmitting_a_trimmed_version_adds_only_the_platforms_it_serves(self):
        trimmed = record("1.0.0", "0.31.0", platforms=(MACOS,))
        newer = record("1.1.0", "0.31.0", platforms=(LINUX,))
        self.assertEqual(submit([trimmed, newer], record("1.0.0", "0.31.0", platforms=(LINUX, MACOS, WINDOWS))),
                         [("csv-loader", "1.0.0", [MACOS, WINDOWS]), ("csv-loader", "1.1.0", [LINUX])])

    def test_resubmitting_a_version_whose_new_platforms_are_superseded_changes_nothing(self):
        extensions = [record("1.0.0", "0.31.0", platforms=(MACOS,)), record("1.1.0", "0.31.0", platforms=(LINUX, WINDOWS))]
        result = update_registry({"extensions": extensions}, record("1.0.0", "0.31.0", platforms=(LINUX, MACOS, WINDOWS)))
        self.assertIs(result["extensions"], extensions)

    def test_resubmitting_a_version_keeps_a_floor_corrected_in_the_registry(self):
        corrected = record("1.0.0", "0.31.0", platforms=(LINUX,))
        result = update_registry({"extensions": [corrected]}, record("1.0.0", "0.28.0", platforms=(LINUX, MACOS)))
        (merged,) = result["extensions"]
        self.assertEqual((merged["min_sdk_required"], sorted(merged["platforms"])), ("0.31.0", [LINUX, MACOS]))

    def test_a_version_without_platforms_is_rejected(self):
        with self.assertRaises(ValueError) as raised:
            update_registry({"extensions": []}, record("1.0.0", "0.28.0", platforms=()))
        self.assertIn("no platform", str(raised.exception))

    def test_a_rejected_submission_leaves_the_registry_untouched(self):
        registry = {"extensions": [record("1.1.0", "0.28.0")]}
        with self.assertRaises(ValueError):
            update_registry(registry, record("1.0.0", "0.28.0"))
        self.assertEqual(registry, {"extensions": [record("1.1.0", "0.28.0")]})

    def test_backfilling_an_older_version_keeps_only_what_it_still_serves(self):
        newer = record("1.1.0", "0.31.0", platforms=(LINUX,))
        self.assertEqual(submit([newer], record("1.0.0", "0.31.0")),
                         [("csv-loader", "1.0.0", [MACOS]), ("csv-loader", "1.1.0", [LINUX])])

    def test_submitting_a_version_that_serves_no_host_is_rejected(self):
        with self.assertRaises(ValueError) as raised:
            update_registry({"extensions": [record("1.1.0", "0.28.0")]}, record("1.0.0", "0.28.0"))
        self.assertIn("1.1.0", str(raised.exception))


class SupersededEntriesTests(unittest.TestCase):
    def test_lists_dropped_records_and_trimmed_platforms(self):
        before = [record("1.0.0", "0.28.0"), record("1.1.0", "0.31.0"), record("0.1.0", "0.28.0", ext_id="mcap-loader")]
        after = update_registry({"extensions": list(before)}, record("1.2.0", "0.31.0", platforms=(LINUX,)))["extensions"]
        self.assertEqual(superseded_entries(before, after), ["csv-loader 1.1.0 (linux-x86_64)"])
        after = update_registry({"extensions": list(before)}, record("1.2.0", "0.28.0"))["extensions"]
        self.assertEqual(
            superseded_entries(before, after),
            ["csv-loader 1.0.0 (linux-x86_64, macos-arm64)", "csv-loader 1.1.0 (linux-x86_64, macos-arm64)"],
        )

    def test_a_plain_addition_supersedes_nothing(self):
        before = [record("1.0.0", "0.28.0")]
        after = update_registry({"extensions": list(before)}, record("2.0.0", "0.34.0"))["extensions"]
        self.assertEqual(superseded_entries(before, after), [])


if __name__ == "__main__":
    unittest.main()
