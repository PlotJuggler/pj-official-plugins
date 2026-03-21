#!/usr/bin/env python3
"""
Package a plugin binary for release.

Finds the compiled binary by its embedded manifest id and copies it
to the distribution directory along with manifest.json.

Usage:
    python3 scripts/package_plugin.py data_load_csv --build-dir build/Release --output-dir dist

Output:
    Prints the ZIP filename to stdout for CI to capture.
"""

import argparse
import shutil
import sys
from pathlib import Path

# Add scripts directory to path for imports
SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))

from pj_validation import find_binary_by_manifest_id, read_manifest


def find_plugin_dir(arg: str) -> Path | None:
    """Find plugin directory by name or manifest id."""
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
        manifest = read_manifest(manifest_path)
        if manifest and manifest.get("id") == arg:
            return plugin_dir

    return None


def main():
    parser = argparse.ArgumentParser(
        description="Package a plugin binary for release",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument("plugin", help="Plugin directory name or manifest id")
    parser.add_argument(
        "--build-dir",
        type=Path,
        required=True,
        help="Directory containing compiled binaries",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="Output directory for packaged files",
    )
    parser.add_argument(
        "--version",
        help="Version string (for ZIP filename)",
    )
    parser.add_argument(
        "--os-label",
        help="OS label for ZIP filename (linux, macos, windows)",
    )
    parser.add_argument(
        "--arch",
        help="Architecture for ZIP filename (x86_64, arm64, etc.)",
    )

    args = parser.parse_args()

    # Find plugin directory
    plugin_dir = find_plugin_dir(args.plugin)
    if not plugin_dir:
        print(f"Error: Plugin not found: {args.plugin}", file=sys.stderr)
        sys.exit(1)

    manifest_path = plugin_dir / "manifest.json"
    manifest = read_manifest(manifest_path)
    if not manifest:
        print(f"Error: Could not read manifest from {manifest_path}", file=sys.stderr)
        sys.exit(1)

    artifact_name = manifest["id"]
    version = args.version or manifest["version"]

    # Create output directory
    output_path = args.output_dir / artifact_name
    output_path.mkdir(parents=True, exist_ok=True)

    # Find and copy binary
    binary = find_binary_by_manifest_id(args.build_dir, artifact_name)
    if not binary:
        print(f"Error: Binary not found for '{artifact_name}' in {args.build_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found binary: {binary}", file=sys.stderr)
    shutil.copy(binary, output_path)
    print(f"Copied to: {output_path / binary.name}", file=sys.stderr)

    # Copy manifest
    shutil.copy(manifest_path, output_path)
    print(f"Copied manifest: {output_path / 'manifest.json'}", file=sys.stderr)

    # Generate ZIP filename if all components provided
    if args.os_label and args.arch:
        zip_name = f"{artifact_name}-{version}-{args.os_label}-{args.arch}.zip"
        print(zip_name)  # stdout for CI to capture
    else:
        print(artifact_name)  # just the artifact name


if __name__ == "__main__":
    main()
