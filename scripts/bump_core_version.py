#!/usr/bin/env python3
"""Bump the pinned plotjuggler_core Conan version across all recipes.

The version is pinned per Conan recipe (root conanfile.txt and each plugin's
conanfile.py) because each is an independent Conan entry point. CMake does not
pin it: find_package(plotjuggler_core CONFIG REQUIRED) resolves whatever Conan
installed. So a core bump only needs to touch the Conan recipes — this script
does that in one command.

Usage:
    # Bump every conanfile to a new version
    python3 scripts/bump_core_version.py 0.2.2

    # Preview the edits without writing
    python3 scripts/bump_core_version.py 0.2.2 --dry-run

Run from the repository root.
"""

import argparse
import re
import sys
from pathlib import Path

# Matches `plotjuggler_core/<version>` where version is e.g. 0.2.1 (optionally
# with a prerelease/build suffix like 0.2.1-rc1). The version is captured so we
# can report the old value.
PIN_RE = re.compile(r"(plotjuggler_core/)([0-9][0-9A-Za-z.\-+]*)")

VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+([0-9A-Za-z.\-+]*)?$")


def recipe_files(root: Path) -> list[Path]:
    files = [root / "conanfile.txt"]
    files += sorted(root.glob("*/conanfile.py"))
    return [f for f in files if f.is_file()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", help="new plotjuggler_core version, e.g. 0.2.2")
    parser.add_argument("--dry-run", action="store_true",
                        help="show what would change without writing")
    args = parser.parse_args()

    if not VERSION_RE.match(args.version):
        parser.error(f"'{args.version}' does not look like a version (expected e.g. 0.2.2)")

    root = Path(__file__).resolve().parent.parent
    new_pin = f"plotjuggler_core/{args.version}"

    changed = 0
    for path in recipe_files(root):
        text = path.read_text()
        if "plotjuggler_core/" not in text:
            continue

        old_versions = {m.group(2) for m in PIN_RE.finditer(text)}
        updated = PIN_RE.sub(rf"\g<1>{args.version}", text)
        if updated == text:
            continue

        rel = path.relative_to(root)
        was = ", ".join(sorted(old_versions))
        print(f"{'[dry-run] ' if args.dry_run else ''}{rel}: {was} -> {args.version}")
        if not args.dry_run:
            path.write_text(updated)
        changed += 1

    if changed == 0:
        print(f"No recipe pinned plotjuggler_core, or all already at {args.version}.")
    else:
        verb = "would update" if args.dry_run else "updated"
        print(f"\n{verb} {changed} recipe(s) to {new_pin}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
