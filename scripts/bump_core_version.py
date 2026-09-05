#!/usr/bin/env python3
"""Manage the single plotjuggler_sdk version pin.

The core version now lives in ONE place: the top-level ``SDK_VERSION`` file (an exact
version, e.g. ``0.5.1``). Every Conan recipe (root ``conanfile.py`` and each plugin's
``conanfile.py``) reads it live; scripts/ensure_core.sh clones the matching ``v<version>``
tag when no prebuilt package is available.

Usage:
    # Show the current pin.
    python3 scripts/bump_core_version.py

    # Set a new exact version: writes SDK_VERSION.
    python3 scripts/bump_core_version.py 0.5.2

    # Preview without writing.
    python3 scripts/bump_core_version.py 0.5.2 --dry-run

    # CI guard: assert SDK_VERSION is an exact version and that no recipe
    # carries a stray literal pin; validate explicit plugin SDK minimums.
    python3 scripts/bump_core_version.py --check

Run from the repository root.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from release_tools import SDK_VERSION_REGEX, validate_manifest_file

ROOT = Path(__file__).resolve().parent.parent
SDK_VERSION_FILE = ROOT / "SDK_VERSION"

# A *literal* plotjuggler_sdk pin — the thing that must NOT reappear in a recipe now
# that the version is read from SDK_VERSION. `f"plotjuggler_sdk/{_SDK_VERSION}"` is fine
# because the quote does not immediately follow the slash.
LITERAL_PIN_RE = re.compile(r'"plotjuggler_sdk/(?:\[[^\]]*\]|[0-9][0-9A-Za-z.\-+]*)"')


def read_sdk_version() -> str:
    return SDK_VERSION_FILE.read_text().strip()


def recipe_files() -> list[Path]:
    files = [ROOT / "conanfile.py"] + sorted(ROOT.glob("*/conanfile.py"))
    return [f for f in files if f.is_file()]


def cmd_check() -> int:
    ok = True
    version = read_sdk_version()
    if not SDK_VERSION_REGEX.fullmatch(version):
        print(f"SDK_VERSION '{version}' is not an exact X.Y.Z version", file=sys.stderr)
        ok = False

    for path in recipe_files():
        for match in LITERAL_PIN_RE.finditer(path.read_text()):
            print(f"{path.relative_to(ROOT)}: stray literal pin {match.group(0)} "
                  f"(recipes must read SDK_VERSION)", file=sys.stderr)
            ok = False

    for path in sorted(ROOT.glob("*/manifest.json")):
        _, errors = validate_manifest_file(path)
        for error in errors:
            print(f"{path.relative_to(ROOT)}: {error}", file=sys.stderr)
            ok = False

    if ok:
        print(f"OK: SDK_VERSION={version}, no stray literal pins, plugin SDK minimums are valid.")
    return 0 if ok else 1


def cmd_set(version: str, dry_run: bool) -> int:
    if not SDK_VERSION_REGEX.fullmatch(version):
        print(f"error: '{version}' is not an exact version (e.g. 0.5.2)", file=sys.stderr)
        return 2

    old = read_sdk_version() if SDK_VERSION_FILE.is_file() else "(none)"
    print(f"{'[dry-run] ' if dry_run else ''}SDK_VERSION: {old} -> {version}")
    if not dry_run:
        SDK_VERSION_FILE.write_text(f"{version}\n")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", nargs="?", help="exact version to set, e.g. 0.5.2")
    parser.add_argument("--check", action="store_true",
                        help="verify SDK_VERSION, recipe pins, and explicit plugin SDK minimums")
    parser.add_argument("--dry-run", action="store_true", help="show without writing")
    args = parser.parse_args()

    if args.check:
        return cmd_check()
    if args.version is None:
        print(read_sdk_version())
        return 0
    return cmd_set(args.version, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
