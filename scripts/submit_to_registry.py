#!/usr/bin/env python3
"""Submit an extension release to pj-plugin-registry as a PR.

Terminology:
- extension: The distributable package (ZIP containing plugin binary + manifest.json)
- plugin: The compiled binary (.so/.dll/.dylib) containing the C++ class
- source_dir: The source directory containing code and manifest.json

Usage:
    python3 scripts/submit_to_registry.py data_load_csv
    python3 scripts/submit_to_registry.py csv-loader
    python3 scripts/submit_to_registry.py data_load_csv --dry-run

Prerequisites:
    - gh CLI authenticated with appropriate permissions
    - Release must already exist on GitHub (use release_plugin.py first)
    - Run from the repository root (pj_official_plugins)
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

# Add scripts directory to path for imports
SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))

from release_tools import (
    compute_sha256_bytes,
    normalize_platform,
    read_manifest,
    semver_sort_key,
    validate_manifest_file,
    VALID_PLATFORMS,
)

SOURCE_REPO = "PlotJuggler/pj-official-plugins"
REGISTRY_REPO = "PlotJuggler/pj-plugin-registry"

PLATFORMS = VALID_PLATFORMS

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
    "min_sdk_required",
    "suggested_sdk_version",
    "floor_test",
    "entry_point",
]


def find_source_dir(arg: str) -> str:
    """Find source directory from argument.

    Accepts:
      - Directory name: data_load_csv
      - Extension id (manifest id): csv-loader
    """
    # Check if it's a direct directory match
    if Path(arg).is_dir() and (Path(arg) / "manifest.json").exists():
        return arg

    # Search all manifests for matching id
    for manifest_path in Path(".").glob("*/manifest.json"):
        manifest = read_manifest(manifest_path)
        if manifest and manifest.get("id") == arg:
            return manifest_path.parent.name

    sys.exit(f"Error: Source directory '{arg}' not found. Provide directory name (e.g. data_load_csv) or extension id (e.g. csv-loader)")


def get_local_tag(tag: str) -> str | None:
    """Check if tag exists locally and return commit sha."""
    result = subprocess.run(
        ["git", "rev-list", "-n", "1", tag],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode == 0:
        return result.stdout.strip()
    return None


def list_github_releases(source_dir: str) -> list[dict]:
    """List all releases for an extension from GitHub."""
    try:
        releases = gh_json(["release", "list", "-R", SOURCE_REPO, "--json", "tagName,publishedAt,isDraft,isPrerelease"])
        # Filter releases for this extension
        prefix = f"{source_dir}/v"
        extension_releases = [r for r in releases if r["tagName"].startswith(prefix)]
        return sorted(extension_releases, key=lambda r: r["publishedAt"], reverse=True)
    except subprocess.CalledProcessError:
        return []


def get_latest_release_version(source_dir: str) -> str | None:
    """Get the latest release version for an extension from GitHub."""
    releases = list_github_releases(source_dir)
    if not releases:
        return None
    # Extract version from tag (e.g., "data_load_csv/v1.0.5" -> "1.0.5")
    tag = releases[0]["tagName"]
    return tag.split("/v")[-1]


def gh_json(args: list[str]) -> dict | list:
    """Run a gh command and parse its JSON output."""
    result = subprocess.run(
        ["gh"] + args,
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def validate_release_exists(tag: str) -> None:
    """Validate that the release exists on GitHub."""
    try:
        gh_json(["release", "view", tag, "-R", SOURCE_REPO, "--json", "tagName"])
    except subprocess.CalledProcessError:
        sys.exit(f"Error: Release '{tag}' not found on GitHub. Wait for CI to complete or check the release.")


def fetch_release_assets(tag: str) -> list[dict]:
    """Fetch release assets for the given tag from GitHub."""
    data = gh_json(["release", "view", tag, "-R", SOURCE_REPO, "--json", "assets"])
    return data.get("assets", [])


def download_and_verify_asset(asset: dict, expected_checksum: str) -> bool:
    """Download asset and verify SHA256 checksum."""
    result = subprocess.run(
        ["gh", "api", asset["apiUrl"], "-H", "Accept: application/octet-stream"],
        capture_output=True,
        check=True,
    )

    # Calculate SHA256 using release_tools
    sha256 = compute_sha256_bytes(result.stdout)

    if sha256 != expected_checksum:
        print(f"    Checksum mismatch for {asset['name']}:")
        print(f"      Expected: {expected_checksum}")
        print(f"      Got:      {sha256}")
        return False

    return True


def download_asset_text(url: str) -> str:
    """Download a release asset's content as text via gh."""
    result = subprocess.run(
        ["gh", "api", url, "-H", "Accept: application/octet-stream"],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def build_platforms(assets: list[dict], extension_id: str, version: str, verify_checksums: bool = True) -> dict:
    """Build the platforms dict from release assets."""
    platforms = {}
    asset_map = {a["name"]: a for a in assets}

    for asset in assets:
        name = asset["name"]
        if not name.startswith(f"{extension_id}-{version}-") or not name.endswith(".zip"):
            continue
        if name.endswith(".sha256"):
            continue

        platform = normalize_platform(name)
        if platform is None or platform not in PLATFORMS:
            print(f"  Warning: skipping unrecognized platform in '{name}'", file=sys.stderr)
            continue

        sha_name = f"{name}.sha256"
        if sha_name not in asset_map:
            print(f"  Warning: no checksum file for '{name}', skipping", file=sys.stderr)
            continue

        sha_asset = asset_map[sha_name]
        sha_content = download_asset_text(sha_asset["apiUrl"])
        checksum = sha_content.strip().split()[0]

        # Verify checksum by downloading and checking
        if verify_checksums:
            print(f"  Verifying {name}...")
            if not download_and_verify_asset(asset, checksum):
                sys.exit(f"Error: Checksum verification failed for {name}")
            print(f"    ✓ Checksum verified")

        platforms[platform] = {
            "url": asset["url"],
            "checksum": f"sha256:{checksum}",
        }

    return platforms


def build_registry_entry(manifest: dict, platforms: dict) -> dict:
    """Build a complete registry extension entry from manifest + platform assets."""
    entry = {}
    for field in MANIFEST_FIELDS:
        if field in manifest:
            entry[field] = manifest[field]

    entry["platforms"] = platforms

    return entry


def fetch_registry_json() -> dict:
    """Fetch current registry.json from the registry repo."""
    result = subprocess.run(
        ["gh", "api", f"repos/{REGISTRY_REPO}/contents/registry.json",
         "--jq", ".content", "-H", "Accept: application/vnd.github.v3+json"],
        capture_output=True,
        text=True,
        check=True,
    )
    import base64
    content = base64.b64decode(result.stdout.strip()).decode("utf-8")
    return json.loads(content)


def prune_registry(extensions: list[dict]) -> list[dict]:
    """Drop the registry records no host would ever pick; sort the rest by (id, version).

    A host admits a record when its min_sdk_required is at most the host SDK
    version, with no upper bound, and installs the highest admitted version
    that ships its platform. So version V is worth keeping for platform P only
    if V's min_sdk_required is strictly lower than that of every newer version
    of the same extension that ships P: one record per SDK floor level, per
    platform. A record keeps only the platforms it still serves and is dropped
    when none is left. A record without min_sdk_required is admitted by every
    host, so it counts as the lowest floor.

    The validator in pj-plugin-registry rejects a registry.json that is not
    pruned by this rule; keep the two in step.
    """
    lowest_newer_floor: dict[tuple[str, str], tuple] = {}
    kept = []
    for ext in sorted(extensions, key=lambda e: (e["id"], semver_sort_key(e["version"])), reverse=True):
        floor = semver_sort_key(ext.get("min_sdk_required", ""))
        platforms = {
            platform: data
            for platform, data in ext["platforms"].items()
            if (ext["id"], platform) not in lowest_newer_floor or floor < lowest_newer_floor[(ext["id"], platform)]
        }
        for platform in platforms:
            lowest_newer_floor[(ext["id"], platform)] = floor
        if platforms:
            kept.append({**ext, "platforms": platforms})
    kept.reverse()
    return kept


def update_registry(registry: dict, entry: dict) -> dict:
    """Add a release to the registry, next to the older versions hosts on an older SDK still need.

    A published version is immutable, with one exception: submitting it again
    may add platforms (a platform build recovered after the release). The
    stored record then gains those platforms and changes in nothing else, so
    metadata corrected by hand in the registry survives. A submission that adds
    nothing returns the registry as it was, with the same "extensions" list.

    Raises ValueError, leaving the registry untouched, when the entry would
    change a published artifact (another URL or checksum for a stored platform,
    or a stored platform gone), or when a newer version already serves every
    platform of the entry at the same or a lower min_sdk_required.
    """
    extension_id, version = entry["id"], entry["version"]
    extensions = registry.get("extensions", [])

    def is_this_version(ext: dict) -> bool:
        return ext.get("id") == extension_id and semver_sort_key(ext["version"]) == semver_sort_key(version)

    stored = next((ext for ext in extensions if is_this_version(ext)), None)
    if stored is not None:
        if stored["version"] != version or any(
            entry["platforms"].get(platform) != data for platform, data in stored["platforms"].items()
        ):
            raise ValueError(
                f"the registry already holds {extension_id} v{stored['version']} with different content. A published "
                "version is immutable: release a new version, or edit registry.json by hand to pull a broken one."
            )
        entry = {**stored, "platforms": {**stored["platforms"], **entry["platforms"]}}
        extensions = [ext for ext in extensions if ext is not stored]

    pruned = prune_registry(extensions + [entry])
    kept = next((ext for ext in pruned if is_this_version(ext)), None)
    if stored is not None and kept == stored:
        return registry
    if kept is None:
        newer = max((ext["version"] for ext in pruned if ext["id"] == extension_id), key=semver_sort_key, default=None)
        reason = (
            f"v{newer} already covers its platforms at the same or a lower min_sdk_required"
            if newer
            else "it ships no platform"
        )
        raise ValueError(f"{extension_id} v{version} would serve no host: {reason}.")

    registry["extensions"] = pruned
    return registry


def superseded_entries(before: list[dict], after: list[dict]) -> list[str]:
    """Describe the platform entries of `before` that `after` no longer holds, one line per record."""
    remaining = {(ext["id"], ext["version"]): ext["platforms"] for ext in after}
    lines = []
    for ext in before:
        gone = sorted(p for p in ext["platforms"] if p not in remaining.get((ext["id"], ext["version"]), {}))
        if gone:
            lines.append(f"{ext['id']} {ext['version']} ({', '.join(gone)})")
    return lines


def create_registry_pr(entry: dict, dry_run: bool = False, release_notes: str | None = None) -> str | None:
    """Create a PR that adds the entry to the registry; return its URL, or None when the registry needs no change."""
    extension_id = entry["id"]
    version = entry["version"]
    branch_name = f"update-{extension_id}-{version}"

    print(f"  Fetching current registry.json...")
    registry = fetch_registry_json()
    before = registry.get("extensions", [])

    print(f"  Updating registry with {extension_id} v{version}...")
    try:
        updated_registry = update_registry(registry, entry)
    except ValueError as e:
        sys.exit(f"Error: {e}")

    if updated_registry.get("extensions", []) == before:
        print(f"  The registry already serves every platform of {extension_id} v{version}: nothing to submit")
        return None

    superseded = superseded_entries(before, updated_registry["extensions"])
    for line in superseded:
        print(f"  Superseded, removed from the registry: {line}")

    if dry_run:
        print(f"\n  [dry-run] Would create PR to update registry")
        print(f"  Branch: {branch_name}")
        return "[dry-run]"

    # Get the SHA of the current registry.json file
    result = subprocess.run(
        ["gh", "api", f"repos/{REGISTRY_REPO}/contents/registry.json",
         "--jq", ".sha", "-H", "Accept: application/vnd.github.v3+json"],
        capture_output=True,
        text=True,
        check=True,
    )
    base_sha = result.stdout.strip()

    # Get default branch
    result = subprocess.run(
        ["gh", "api", f"repos/{REGISTRY_REPO}", "--jq", ".default_branch"],
        capture_output=True,
        text=True,
        check=True,
    )
    default_branch = result.stdout.strip()

    # Get the SHA of the default branch
    result = subprocess.run(
        ["gh", "api", f"repos/{REGISTRY_REPO}/git/ref/heads/{default_branch}",
         "--jq", ".object.sha"],
        capture_output=True,
        text=True,
        check=True,
    )
    branch_sha = result.stdout.strip()

    # Delete branch if it already exists (from a previous failed attempt)
    subprocess.run(
        ["gh", "api", f"repos/{REGISTRY_REPO}/git/refs/heads/{branch_name}",
         "-X", "DELETE"],
        capture_output=True,
        text=True,
        check=False,  # Ignore errors if branch doesn't exist
    )

    # Create branch
    print(f"  Creating branch {branch_name}...")
    subprocess.run(
        ["gh", "api", f"repos/{REGISTRY_REPO}/git/refs", "-X", "POST",
         "-f", f"ref=refs/heads/{branch_name}",
         "-f", f"sha={branch_sha}"],
        capture_output=True,
        text=True,
        check=True,
    )

    # Update registry.json in the branch
    import base64
    content_b64 = base64.b64encode(
        json.dumps(updated_registry, indent=2).encode("utf-8")
    ).decode("utf-8")

    print(f"  Committing registry.json update...")
    subprocess.run(
        ["gh", "api", f"repos/{REGISTRY_REPO}/contents/registry.json", "-X", "PUT",
         "-f", f"message=Update {extension_id} to v{version}",
         "-f", f"content={content_b64}",
         "-f", f"branch={branch_name}",
         "-f", f"sha={base_sha}"],
        capture_output=True,
        text=True,
        check=True,
    )

    # Create PR
    print(f"  Creating PR...")
    stored_platforms = next(
        ext["platforms"]
        for ext in updated_registry["extensions"]
        if ext["id"] == extension_id and ext["version"] == version
    )
    platform_summary = ", ".join(sorted(stored_platforms))
    pr_body = (
        f"## Update `{extension_id}` to v{version}\n\n"
        f"**Platforms:** {platform_summary}\n\n"
        f"This PR adds v{version} of extension `{extension_id}` to the registry, or platforms to its record."
    )
    if superseded:
        pr_body += "\n\n**Removed, because no PlotJuggler would pick them any more:**\n\n"
        pr_body += "\n".join(f"- `{line}`" for line in superseded)
    else:
        pr_body += "\n\nNo other record changes."
    if release_notes:
        pr_body += f"\n\n## Release Notes\n\n{release_notes.strip()}"
    pr_body += "\n\n---\n*Generated by submit_to_registry.py*"

    result = subprocess.run(
        ["gh", "pr", "create",
         "--repo", REGISTRY_REPO,
         "--head", branch_name,
         "--base", default_branch,
         "--title", f"Update {extension_id} to v{version}",
         "--body", pr_body],
        capture_output=True,
        text=True,
        check=True,
    )

    return result.stdout.strip()


def main():
    global SOURCE_REPO

    parser = argparse.ArgumentParser(
        description="Submit an extension release to pj-plugin-registry.",
        epilog="Note: Release must already exist on GitHub. Use release_plugin.py to create it first.",
    )
    parser.add_argument(
        "source",
        help="Source directory (e.g. data_load_csv) or extension id (e.g. csv-loader)",
    )
    parser.add_argument(
        "--version", "-v",
        metavar="VERSION",
        help="Version to submit (e.g. 1.0.5). If not specified, uses latest release from GitHub.",
    )
    parser.add_argument(
        "--list-releases", "-l",
        action="store_true",
        help="List available releases for the extension and exit",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the registry entry JSON without creating an issue",
    )
    parser.add_argument(
        "--skip-checksum-verify",
        action="store_true",
        help="Skip downloading and verifying asset checksums",
    )
    parser.add_argument(
        "--releases-repo",
        default=SOURCE_REPO,
        help=f"GitHub repo for fetching releases (default: {SOURCE_REPO})",
    )
    parser.add_argument(
        "--release-notes",
        type=Path,
        help="Markdown release notes to include in the registry PR body",
    )
    args = parser.parse_args()

    SOURCE_REPO = args.releases_repo

    # Find source directory
    source_dir = find_source_dir(args.source)
    manifest_path = Path(source_dir) / "manifest.json"

    # Read and validate manifest
    manifest, validation_errors = validate_manifest_file(manifest_path)
    if manifest is None:
        sys.exit(f"Error: Could not read {manifest_path}")
    if validation_errors:
        print(f"Error: Manifest validation failed:", file=sys.stderr)
        for err in validation_errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)

    extension_id = manifest["id"]

    print(f"Source: {source_dir}")
    print(f"Extension: {extension_id}")
    print(f"Source repo: {SOURCE_REPO}")

    # List releases if requested
    if args.list_releases:
        print(f"\nAvailable releases for {source_dir}:")
        releases = list_github_releases(source_dir)
        if not releases:
            print("  No releases found")
        else:
            for r in releases:
                tag = r["tagName"]
                version = tag.split("/v")[-1]
                date = r["publishedAt"][:10]
                flags = []
                if r.get("isDraft"):
                    flags.append("draft")
                if r.get("isPrerelease"):
                    flags.append("prerelease")
                flag_str = f" ({', '.join(flags)})" if flags else ""
                print(f"  {version} - {date}{flag_str}")
        return

    # Determine version
    if args.version:
        version = args.version
        print(f"\nUsing specified version: {version}")
    else:
        print(f"\nFetching latest release from GitHub...")
        version = get_latest_release_version(source_dir)
        if not version:
            sys.exit(f"Error: No releases found for {source_dir} on {SOURCE_REPO}. Specify --version or create a release first.")
        print(f"  Latest version: {version}")

    tag = f"{source_dir}/v{version}"
    print(f"Tag: {tag}")

    # The registry entry takes its version and min_sdk_required from the manifest on disk.
    if version != manifest["version"]:
        sys.exit(
            f"Error: release v{version} does not match the manifest version {manifest['version']} in "
            f"{manifest_path}. Check out the release tag first: git checkout {tag}"
        )

    # Check if tag exists locally (warning only, not required)
    local_commit = get_local_tag(tag)
    if local_commit:
        print(f"\nLocal tag: exists at {local_commit[:12]}")
    else:
        print(f"\nLocal tag: not found (will use GitHub release)")

    # Validate release exists on GitHub
    print(f"\nValidating release on GitHub...")
    validate_release_exists(tag)
    print(f"  ✓ Release exists on {SOURCE_REPO}")

    # Fetch release assets
    print(f"\nFetching release assets...")
    assets = fetch_release_assets(tag)
    if not assets:
        sys.exit(f"Error: No assets found for release '{tag}'")
    print(f"  Found {len(assets)} assets")

    # Build platforms (with checksum verification)
    print(f"\nBuilding platform entries...")
    platforms = build_platforms(
        assets, extension_id, version,
        verify_checksums=not args.skip_checksum_verify
    )
    if not platforms:
        sys.exit("Error: No valid platform artifacts found")

    missing = set(PLATFORMS) - set(platforms.keys())
    if missing:
        print(f"  Warning: missing platforms: {', '.join(sorted(missing))}", file=sys.stderr)

    # Build registry entry
    entry = build_registry_entry(manifest, platforms)
    release_notes = None
    if args.release_notes:
        try:
            release_notes = args.release_notes.read_text()
        except OSError as e:
            sys.exit(f"Error: Could not read release notes '{args.release_notes}': {e}")

    if args.dry_run:
        print(f"\nRegistry entry (dry-run):")
        print(json.dumps(entry, indent=2))
        if release_notes:
            print(f"\nRelease notes (dry-run):")
            print(release_notes)

    # Create PR
    print(f"\nCreating PR on {REGISTRY_REPO}...")
    pr_url = create_registry_pr(entry, dry_run=args.dry_run, release_notes=release_notes)

    if pr_url is None:
        print("\n✓ Registry already up to date, no PR created")
    elif not args.dry_run:
        print(f"\n✓ PR created: {pr_url}")


if __name__ == "__main__":
    main()
