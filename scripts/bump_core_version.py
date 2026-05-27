#!/usr/bin/env python3
"""Set the plotjuggler_core Conan requirement across all recipes.

The requirement is declared per Conan recipe (root conanfile.txt and each
plugin's conanfile.py) because each is an independent Conan entry point. CMake
does not pin it: find_package(plotjuggler_core CONFIG REQUIRED) resolves
whatever Conan installed. So changing the core requirement only needs to touch
the Conan recipes — this script does that in one command.

The requirement is normally a patch-level version range, so new core patch
releases are picked up automatically without editing any recipe.

Usage:
    # Patch range (recommended): "0.3" -> plotjuggler_core/[~0.3]
    # i.e. >=0.3.0 <0.4.0, any 0.3.x patch.
    python3 scripts/bump_core_version.py 0.3

    # Exact pin: a full version stays exact.
    python3 scripts/bump_core_version.py 0.3.0

    # Literal range, verbatim (quote it for the shell).
    python3 scripts/bump_core_version.py '[>=0.3.0 <0.5.0]'

    # Preview without writing.
    python3 scripts/bump_core_version.py 0.3 --dry-run

Run from the repository root.
"""

import argparse
import re
import sys
from pathlib import Path

# Matches `plotjuggler_core/<spec>` where <spec> is either a bracketed version
# range (e.g. [~0.3]) or a bare version (e.g. 0.2.1, 0.2.1-rc1). The spec is
# captured so we can report the old value and replace it in place.
PIN_RE = re.compile(r"(plotjuggler_core/)(\[[^\]]*\]|[0-9][0-9A-Za-z.\-+]*)")

MINOR_RE = re.compile(r"^[0-9]+\.[0-9]+$")
EXACT_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+([0-9A-Za-z.\-+]*)?$")


def to_spec(arg: str) -> str:
    """Turn a CLI argument into a Conan requirement spec.

    MAJOR.MINOR  -> tilde range [~MAJOR.MINOR] (any patch in that minor)
    full version -> exact pin, unchanged
    [..]         -> literal range, used verbatim
    """
    if arg.startswith("["):
        if not arg.endswith("]"):
            raise ValueError(f"unbalanced range '{arg}' (expected a closing ']')")
        return arg
    if MINOR_RE.match(arg):
        return f"[~{arg}]"
    if EXACT_RE.match(arg):
        return arg
    raise ValueError(
        f"'{arg}' is neither MAJOR.MINOR (e.g. 0.3), a full version "
        f"(e.g. 0.3.0), nor a [..] range")


def recipe_files(root: Path) -> list[Path]:
    files = [root / "conanfile.txt"]
    files += sorted(root.glob("*/conanfile.py"))
    return [f for f in files if f.is_file()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("spec", help="MAJOR.MINOR (range), full version (exact), or a [..] range")
    parser.add_argument("--dry-run", action="store_true",
                        help="show what would change without writing")
    args = parser.parse_args()

    try:
        spec = to_spec(args.spec)
    except ValueError as exc:
        parser.error(str(exc))

    root = Path(__file__).resolve().parent.parent
    new_pin = f"plotjuggler_core/{spec}"

    changed = 0
    for path in recipe_files(root):
        text = path.read_text()
        if "plotjuggler_core/" not in text:
            continue

        old_specs = {m.group(2) for m in PIN_RE.finditer(text)}
        updated = PIN_RE.sub(lambda m: m.group(1) + spec, text)
        if updated == text:
            continue

        rel = path.relative_to(root)
        was = ", ".join(sorted(old_specs))
        print(f"{'[dry-run] ' if args.dry_run else ''}{rel}: {was} -> {spec}")
        if not args.dry_run:
            path.write_text(updated)
        changed += 1

    if changed == 0:
        print(f"No recipe pinned plotjuggler_core, or all already at {spec}.")
    else:
        verb = "would update" if args.dry_run else "updated"
        print(f"\n{verb} {changed} recipe(s) to {new_pin}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
