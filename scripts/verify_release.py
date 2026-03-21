#!/usr/bin/env python3
"""
Verify plugin release consistency.

This CLI tool validates that tag, manifest.json, and compiled binary
all have matching versions. Designed for use in CI workflows after
building plugin artifacts.

Usage:
    # Verify all versions match
    python3 scripts/verify_release.py data_load_csv --binary build/Release/libDataLoadCSV.so

    # Verify against expected version (from tag)
    python3 scripts/verify_release.py data_load_csv --expected-version 1.0.5

    # Validate manifest only
    python3 scripts/verify_release.py data_load_csv --check-manifest

    # Full verification in CI
    python3 scripts/verify_release.py data_load_csv \
        --binary dist/csv-loader/libDataLoadCSV.so \
        --expected-version 1.0.5 \
        --check-manifest
"""

import argparse
import sys
from pathlib import Path

# Add scripts directory to path for imports
SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))

from pj_validation import (
    extract_binary_version,
    extract_binary_manifest,
    read_manifest_version,
    read_manifest,
    validate_manifest_file,
    validate_semver,
    find_binary_by_manifest_id,
)


def find_plugin_dir(arg: str) -> Path | None:
    """
    Find plugin directory by name or manifest id.

    Args:
        arg: Plugin directory name or manifest id

    Returns:
        Path to plugin directory or None
    """
    root = SCRIPT_DIR.parent

    # Try direct directory name
    direct = root / arg
    if direct.is_dir() and (direct / "manifest.json").exists():
        return direct

    # Search all plugin directories for matching manifest id
    for plugin_dir in root.iterdir():
        if not plugin_dir.is_dir():
            continue
        manifest_path = plugin_dir / "manifest.json"
        if not manifest_path.exists():
            continue
        try:
            import json

            with open(manifest_path) as f:
                manifest = json.load(f)
            if manifest.get("id") == arg:
                return plugin_dir
        except Exception:
            continue

    return None


def main():
    parser = argparse.ArgumentParser(
        description="Verify plugin release version consistency",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    parser.add_argument("plugin", help="Plugin directory name or manifest id")

    parser.add_argument(
        "--binary",
        "-b",
        type=Path,
        help="Path to compiled plugin binary (.so/.dll/.dylib)",
    )

    parser.add_argument(
        "--expected-version",
        "-e",
        help="Expected version to verify against (e.g., from tag)",
    )

    parser.add_argument(
        "--check-manifest",
        "-m",
        action="store_true",
        help="Validate manifest.json structure and fields",
    )

    parser.add_argument(
        "--check-binary-manifest",
        action="store_true",
        help="Extract and display full manifest from binary",
    )

    parser.add_argument(
        "--find-binary",
        type=Path,
        help="Directory to search for plugin binaries",
    )

    parser.add_argument(
        "--quiet",
        "-q",
        action="store_true",
        help="Only output errors",
    )

    args = parser.parse_args()

    # Find plugin directory
    plugin_dir = find_plugin_dir(args.plugin)
    if not plugin_dir:
        print(f"Error: Plugin not found: {args.plugin}", file=sys.stderr)
        sys.exit(1)

    manifest_path = plugin_dir / "manifest.json"
    errors = []
    versions = {}

    if not args.quiet:
        print(f"Plugin: {plugin_dir.name}")
        print()

    # === Validate manifest structure ===
    if args.check_manifest:
        if not args.quiet:
            print("Validating manifest.json...")
        manifest, manifest_errors = validate_manifest_file(manifest_path)
        if manifest_errors:
            for err in manifest_errors:
                print(f"  ERROR: {err}", file=sys.stderr)
            errors.extend(manifest_errors)
        elif not args.quiet:
            print(f"  OK: Manifest valid")
        print()

    # === Read manifest ===
    manifest = read_manifest(manifest_path)
    if not manifest:
        print(f"Error: Could not read manifest from {manifest_path}", file=sys.stderr)
        sys.exit(1)

    manifest_id = manifest.get("id")
    manifest_version = manifest.get("version")

    if manifest_version:
        versions["manifest"] = manifest_version
        if not args.quiet:
            print(f"Manifest version: {manifest_version}")
            print(f"Manifest id:      {manifest_id}")
    else:
        errors.append(f"Could not read manifest version from {manifest_path}")

    # === Find binary if directory specified ===
    binary_path = args.binary
    if args.find_binary and not binary_path:
        if not args.quiet:
            print(f"\nSearching for binary with manifest id '{manifest_id}' in {args.find_binary}...")
        binary_path = find_binary_by_manifest_id(args.find_binary, manifest_id)
        if binary_path:
            if not args.quiet:
                print(f"Found binary: {binary_path}")
        else:
            if not args.quiet:
                print(f"No binary found with manifest id '{manifest_id}' in {args.find_binary}")

    # === Extract binary version ===
    if binary_path:
        if not binary_path.exists():
            errors.append(f"Binary not found: {binary_path}")
        else:
            binary_version = extract_binary_version(binary_path)
            if binary_version:
                versions["binary"] = binary_version
                if not args.quiet:
                    print(f"Binary version:   {binary_version}")
            else:
                errors.append(f"Could not extract version from binary: {binary_path}")

            # Show full binary manifest if requested
            if args.check_binary_manifest:
                print()
                print("Binary embedded manifest:")
                binary_manifest = extract_binary_manifest(binary_path)
                if binary_manifest:
                    import json

                    print(json.dumps(binary_manifest, indent=2))
                else:
                    print("  (could not extract)")

    # === Expected version ===
    if args.expected_version:
        versions["expected"] = args.expected_version
        if not args.quiet:
            print(f"Expected version: {args.expected_version}")

    # === Compare versions ===
    print()
    if len(versions) >= 2:
        unique = set(versions.values())
        if len(unique) == 1:
            if not args.quiet:
                print("OK: All versions match")
        else:
            print("ERROR: Version mismatch!", file=sys.stderr)
            for source, ver in versions.items():
                print(f"  {source}: {ver}", file=sys.stderr)
            errors.append("Version mismatch")
    elif len(versions) == 1:
        if not args.quiet:
            print("Note: Only one version source available, nothing to compare")

    # === Validate semver format ===
    for source, ver in versions.items():
        if not validate_semver(ver):
            errors.append(f"Invalid semver format in {source}: {ver}")

    # === Summary ===
    if errors:
        print()
        print(f"FAILED: {len(errors)} error(s)", file=sys.stderr)
        sys.exit(1)
    else:
        if not args.quiet:
            print()
            print("PASSED: All checks successful")
        sys.exit(0)


if __name__ == "__main__":
    main()
