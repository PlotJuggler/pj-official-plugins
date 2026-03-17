#!/usr/bin/env python3
"""Submit a plugin release to pj-plugin-registry as a GitHub issue.

Usage:
    python3 scripts/submit_to_registry.py data_load_csv/v1.0.5
    python3 scripts/submit_to_registry.py data_load_csv/v1.0.5 --dry-run

Prerequisites:
    - gh CLI authenticated with appropriate permissions
    - Run from the repository root (pj_official_plugins)
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SOURCE_REPO = "PlotJuggler/pj-official-plugins"
REGISTRY_REPO = "PlotJuggler/pj-plugin-registry"

# Registry platform keys.  The workflow matrix produces os_label-arch combos;
# normalize the ones that differ from the registry convention.
ARCH_NORMALIZE = {"x64": "x86_64"}

PLATFORMS = [
    "linux-x86_64",
    "linux-aarch64",
    "macos-x86_64",
    "macos-arm64",
    "windows-x86_64",
    "windows-arm64",
]

# Fields copied verbatim from manifest.json into the registry entry.
MANIFEST_FIELDS = [
    "id",
    "name",
    "version",
    "description",
    "author",
    "publisher",
    "website",
    "repository",
    "license",
    "icon_url",
    "category",
    "tags",
    "min_plotjuggler_version",
]


def parse_tag(tag: str) -> tuple[str, str]:
    """Parse 'data_load_csv/v1.0.5' → ('data_load_csv', '1.0.5')."""
    match = re.match(r"^([^/]+)/v(.+)$", tag)
    if not match:
        sys.exit(f"Error: tag '{tag}' does not match pattern '<plugin_dir>/v<version>'")
    return match.group(1), match.group(2)


def read_manifest(plugin_dir: str) -> dict:
    manifest_path = Path(plugin_dir) / "manifest.json"
    if not manifest_path.exists():
        sys.exit(f"Error: {manifest_path} not found")
    with open(manifest_path) as f:
        return json.load(f)


def gh_json(args: list[str]) -> dict | list:
    """Run a gh command and parse its JSON output."""
    result = subprocess.run(
        ["gh"] + args,
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def fetch_release_assets(tag: str) -> list[dict]:
    """Fetch release assets for the given tag from GitHub."""
    data = gh_json(["release", "view", tag, "-R", SOURCE_REPO, "--json", "assets"])
    return data.get("assets", [])


def download_asset_text(url: str) -> str:
    """Download a release asset's content as text via gh."""
    result = subprocess.run(
        ["gh", "api", url, "-H", "Accept: application/octet-stream"],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def normalize_platform(filename: str) -> str | None:
    """Extract and normalize the platform key from an artifact filename.

    E.g. 'csv-loader-1.0.5-windows-x64.zip' → 'windows-x86_64'
    """
    # Match: name-version-os-arch.zip
    match = re.search(r"-([a-z]+)-([a-z0-9_]+)\.zip$", filename)
    if not match:
        return None
    os_label = match.group(1)
    arch = ARCH_NORMALIZE.get(match.group(2), match.group(2))
    return f"{os_label}-{arch}"


def build_platforms(assets: list[dict], artifact_name: str, version: str) -> dict:
    """Build the platforms dict from release assets."""
    platforms = {}

    # Index assets by name for quick lookup
    asset_map = {a["name"]: a for a in assets}

    for asset in assets:
        name = asset["name"]
        if not name.startswith(f"{artifact_name}-{version}-") or not name.endswith(".zip"):
            continue
        if name.endswith(".sha256"):
            continue

        platform = normalize_platform(name)
        if platform is None or platform not in PLATFORMS:
            print(f"  Warning: skipping unrecognized platform in '{name}'", file=sys.stderr)
            continue

        # Get checksum from .sha256 asset
        sha_name = f"{name}.sha256"
        if sha_name not in asset_map:
            print(f"  Warning: no checksum file for '{name}', skipping", file=sys.stderr)
            continue

        sha_asset = asset_map[sha_name]
        sha_content = download_asset_text(sha_asset["apiUrl"])
        # .sha256 format: "<hash>  <filename>" or "<hash> <filename>"
        checksum = sha_content.strip().split()[0]

        platforms[platform] = {
            "url": asset["url"],
            "checksum": f"sha256:{checksum}",
        }

    return platforms


def build_registry_entry(manifest: dict, platforms: dict) -> dict:
    """Build a complete registry entry from manifest + platform artifacts."""
    entry = {}
    for field in MANIFEST_FIELDS:
        if field in manifest:
            entry[field] = manifest[field]
    entry["platforms"] = platforms
    return entry


def create_issue(entry: dict) -> str:
    """Create a GitHub issue on pj-plugin-registry with the registry entry."""
    ext_id = entry["id"]
    version = entry["version"]
    title = f"Update {ext_id} to {version}"

    platform_summary = ", ".join(sorted(entry.get("platforms", {}).keys()))
    body = (
        f"## Registry entry for `{ext_id}` v{version}\n\n"
        f"**Platforms:** {platform_summary}\n\n"
        f"Please add or update the following entry in `registry.json`:\n\n"
        f"```json\n{json.dumps(entry, indent=2)}\n```\n"
    )

    result = subprocess.run(
        ["gh", "issue", "create", "-R", REGISTRY_REPO, "--title", title, "--body", body],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description="Submit a plugin release to pj-plugin-registry.")
    parser.add_argument("tag", help="Release tag, e.g. data_load_csv/v1.0.5")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the registry entry JSON without creating an issue",
    )
    parser.add_argument(
        "--repo",
        default=SOURCE_REPO,
        help=f"Source repo (default: {SOURCE_REPO})",
    )
    args = parser.parse_args()

    global SOURCE_REPO
    SOURCE_REPO = args.repo

    plugin_dir, version = parse_tag(args.tag)
    print(f"Plugin: {plugin_dir}, Version: {version}")

    # Read and validate manifest
    manifest = read_manifest(plugin_dir)

    manifest_version = manifest.get("version", "")
    if manifest_version != version:
        sys.exit(
            f"Error: version mismatch — tag says '{version}' "
            f"but manifest.json says '{manifest_version}'"
        )

    artifact_name = manifest.get("id", plugin_dir)
    print(f"Artifact name: {artifact_name}")

    # Fetch release assets
    print(f"Fetching release assets for {args.tag}...")
    assets = fetch_release_assets(args.tag)
    if not assets:
        sys.exit(f"Error: no assets found for release '{args.tag}'")
    print(f"  Found {len(assets)} assets")

    # Build platforms
    platforms = build_platforms(assets, artifact_name, version)
    if not platforms:
        sys.exit("Error: no valid platform artifacts found")

    missing = set(PLATFORMS) - set(platforms.keys())
    if missing:
        print(f"  Warning: missing platforms: {', '.join(sorted(missing))}", file=sys.stderr)

    # Build registry entry
    entry = build_registry_entry(manifest, platforms)

    if args.dry_run:
        print(json.dumps(entry, indent=2))
        return

    # Create issue
    print("Creating issue on pj-plugin-registry...")
    issue_url = create_issue(entry)
    print(f"Issue created: {issue_url}")


if __name__ == "__main__":
    main()
