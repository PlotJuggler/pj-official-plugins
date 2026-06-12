#!/usr/bin/env python3
"""Manage the single plotjuggler_sdk version pin.

The core version now lives in ONE place: the top-level ``SDK_VERSION`` file (an exact
version, e.g. ``0.5.1``). Every Conan recipe (root ``conanfile.py`` and each plugin's
``conanfile.py``) reads it live, and the ``extern/plotjuggler_core`` git submodule is
pinned to the matching ``v<version>`` tag. This script keeps those in sync.

Usage:
    # Show the current pin.
    python3 scripts/bump_core_version.py

    # Set a new exact version: writes SDK_VERSION and moves the submodule to vX.Y.Z.
    python3 scripts/bump_core_version.py 0.5.2

    # Preview without writing / touching the submodule.
    python3 scripts/bump_core_version.py 0.5.2 --dry-run

    # Only update SDK_VERSION, leave the submodule alone.
    python3 scripts/bump_core_version.py 0.5.2 --no-submodule

    # CI guard: assert SDK_VERSION matches the submodule tag and that no recipe
    # carries a stray literal pin. Exit non-zero on any drift.
    python3 scripts/bump_core_version.py --check

Run from the repository root.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK_VERSION_FILE = ROOT / "SDK_VERSION"
SUBMODULE = ROOT / "extern" / "plotjuggler_core"

EXACT_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+([0-9A-Za-z.\-+]*)?$")
# A *literal* plotjuggler_sdk pin — the thing that must NOT reappear in a recipe now
# that the version is read from SDK_VERSION. `f"plotjuggler_sdk/{_SDK_VERSION}"` is fine
# because the quote does not immediately follow the slash.
LITERAL_PIN_RE = re.compile(r'"plotjuggler_sdk/(?:\[[^\]]*\]|[0-9][0-9A-Za-z.\-+]*)"')


def read_sdk_version() -> str:
    return SDK_VERSION_FILE.read_text().strip()


def recipe_files() -> list[Path]:
    files = [ROOT / "conanfile.py"] + sorted(ROOT.glob("*/conanfile.py"))
    return [f for f in files if f.is_file()]


def submodule_tag() -> str | None:
    try:
        out = subprocess.run(
            ["git", "-C", str(SUBMODULE), "describe", "--tags", "--exact-match"],
            capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def cmd_check() -> int:
    ok = True
    version = read_sdk_version()
    if not EXACT_RE.match(version):
        print(f"SDK_VERSION '{version}' is not an exact X.Y.Z version", file=sys.stderr)
        ok = False

    tag = submodule_tag()
    if tag is None:
        print("could not read extern/plotjuggler_core tag (submodule not initialized?)",
              file=sys.stderr)
        ok = False
    elif tag != f"v{version}":
        print(f"submodule tag {tag} != v{version} (SDK_VERSION)", file=sys.stderr)
        ok = False

    for path in recipe_files():
        for match in LITERAL_PIN_RE.finditer(path.read_text()):
            print(f"{path.relative_to(ROOT)}: stray literal pin {match.group(0)} "
                  f"(recipes must read SDK_VERSION)", file=sys.stderr)
            ok = False

    if ok:
        print(f"OK: SDK_VERSION={version}, submodule={tag}, no stray literal pins.")
    return 0 if ok else 1


def cmd_set(version: str, dry_run: bool, no_submodule: bool) -> int:
    if not EXACT_RE.match(version):
        print(f"error: '{version}' is not an exact version (e.g. 0.5.2)", file=sys.stderr)
        return 2

    old = read_sdk_version() if SDK_VERSION_FILE.is_file() else "(none)"
    print(f"{'[dry-run] ' if dry_run else ''}SDK_VERSION: {old} -> {version}")
    if not dry_run:
        SDK_VERSION_FILE.write_text(f"{version}\n")

    if no_submodule:
        return 0

    tag = f"v{version}"
    print(f"{'[dry-run] ' if dry_run else ''}submodule extern/plotjuggler_core -> {tag}")
    if not dry_run:
        subprocess.run(["git", "-C", str(SUBMODULE), "fetch", "--tags", "--quiet"], check=True)
        subprocess.run(["git", "-C", str(SUBMODULE), "checkout", "--quiet", tag], check=True)
        subprocess.run(["git", "-C", str(ROOT), "add", "SDK_VERSION", "extern/plotjuggler_core"],
                       check=True)
        print("staged SDK_VERSION + submodule pointer; review and commit.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", nargs="?", help="exact version to set, e.g. 0.5.2")
    parser.add_argument("--check", action="store_true",
                        help="verify SDK_VERSION matches the submodule and no stray pins")
    parser.add_argument("--dry-run", action="store_true", help="show without writing")
    parser.add_argument("--no-submodule", action="store_true",
                        help="update SDK_VERSION only; do not move the submodule")
    args = parser.parse_args()

    if args.check:
        return cmd_check()
    if args.version is None:
        print(read_sdk_version())
        return 0
    return cmd_set(args.version, args.dry_run, args.no_submodule)


if __name__ == "__main__":
    sys.exit(main())
