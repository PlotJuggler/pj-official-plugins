#!/usr/bin/env python3
"""Submit a plugin release to pj-plugin-registry as a GitHub issue.

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
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import git

SOURCE_REPO = "PlotJuggler/pj-official-plugins"
REGISTRY_REPO = "PlotJuggler/pj-plugin-registry"

# Map CI artifact arch names to registry platform keys
ARCH_NORMALIZE = {
    "x64": "x86_64",
    "aarch64": "arm64",  # CI uses aarch64, registry uses arm64
}

PLATFORMS = [
    "linux-x86_64",
    "linux-arm64",
    "macos-x86_64",
    "macos-arm64",
    "windows-x86_64",
    "windows-arm64",
]

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


def id_to_class_name(plugin_id: str) -> str:
    """Convert plugin id to class name (kebab-case to PascalCase)."""
    # csv-loader -> CsvLoader
    return "".join(word.capitalize() for word in plugin_id.split("-"))


def find_plugin_dir(arg: str) -> str:
    """Find plugin directory from argument.

    Accepts:
      - Directory name: data_load_csv
      - Manifest id: csv-loader
    """
    # Check if it's a direct directory match
    if Path(arg).is_dir() and (Path(arg) / "manifest.json").exists():
        return arg

    # Search all manifests for matching id
    for manifest_path in Path(".").glob("*/manifest.json"):
        try:
            with open(manifest_path) as f:
                manifest = json.load(f)
                if manifest.get("id") == arg:
                    return manifest_path.parent.name
        except (json.JSONDecodeError, IOError):
            continue

    sys.exit(f"Error: Plugin '{arg}' not found. Provide directory name (e.g. data_load_csv) or manifest id (e.g. csv-loader)")


def read_manifest(plugin_dir: str) -> dict:
    """Read and validate manifest.json."""
    manifest_path = Path(plugin_dir) / "manifest.json"
    if not manifest_path.exists():
        sys.exit(f"Error: {manifest_path} not found")
    with open(manifest_path) as f:
        manifest = json.load(f)

    # Validate required fields
    if "version" not in manifest:
        sys.exit(f"Error: 'version' not found in {manifest_path}")
    if "id" not in manifest:
        sys.exit(f"Error: 'id' not found in {manifest_path}")

    return manifest


def get_local_tag(tag: str) -> str | None:
    """Check if tag exists locally and return commit sha."""
    repo = git.Repo(".")
    if tag in [t.name for t in repo.tags]:
        return repo.tags[tag].commit.hexsha
    return None


def list_github_releases(plugin_dir: str) -> list[dict]:
    """List all releases for a plugin from GitHub."""
    try:
        releases = gh_json(["release", "list", "-R", SOURCE_REPO, "--json", "tagName,publishedAt,isDraft,isPrerelease"])
        # Filter releases for this plugin
        prefix = f"{plugin_dir}/v"
        plugin_releases = [r for r in releases if r["tagName"].startswith(prefix)]
        return sorted(plugin_releases, key=lambda r: r["publishedAt"], reverse=True)
    except subprocess.CalledProcessError:
        return []


def get_latest_release_version(plugin_dir: str) -> str | None:
    """Get the latest release version for a plugin from GitHub."""
    releases = list_github_releases(plugin_dir)
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
    with tempfile.NamedTemporaryFile(delete=True) as tmp:
        result = subprocess.run(
            ["gh", "api", asset["apiUrl"], "-H", "Accept: application/octet-stream"],
            capture_output=True,
            check=True,
        )
        tmp.write(result.stdout)
        tmp.flush()

        # Calculate SHA256
        sha256 = hashlib.sha256(result.stdout).hexdigest()

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


def normalize_platform(filename: str) -> str | None:
    """Extract and normalize the platform key from an artifact filename."""
    match = re.search(r"-([a-z]+)-([a-z0-9_]+)\.zip$", filename)
    if not match:
        return None
    os_label = match.group(1)
    arch = ARCH_NORMALIZE.get(match.group(2), match.group(2))
    return f"{os_label}-{arch}"


def build_platforms(assets: list[dict], artifact_name: str, version: str, verify_checksums: bool = True) -> dict:
    """Build the platforms dict from release assets."""
    platforms = {}
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
    """Build a complete registry entry from manifest + platform artifacts."""
    entry = {}
    for field in MANIFEST_FIELDS:
        if field in manifest:
            entry[field] = manifest[field]

    # Add plugins array (from manifest or generate default)
    if "plugins" in manifest:
        entry["plugins"] = manifest["plugins"]
    else:
        # Generate default plugins entry
        plugin_id = manifest["id"]
        category = manifest.get("category", "parser")
        entry["plugins"] = [
            {
                "name": id_to_class_name(plugin_id),
                "type": category,
                "library": plugin_id,
            }
        ]

    entry["platforms"] = platforms

    # Add changelog (from manifest or generate default)
    if "changelog" in manifest:
        entry["changelog"] = manifest["changelog"]
    else:
        version = manifest["version"]
        entry["changelog"] = {
            version: f"Release v{version}"
        }

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


def update_registry(registry: dict, entry: dict) -> dict:
    """Update registry with new entry (add or update existing)."""
    extensions = registry.get("extensions", [])

    # Find existing entry by id
    existing_idx = None
    for i, ext in enumerate(extensions):
        if ext.get("id") == entry["id"]:
            existing_idx = i
            break

    if existing_idx is not None:
        # Update existing entry
        extensions[existing_idx] = entry
    else:
        # Add new entry
        extensions.append(entry)

    registry["extensions"] = extensions
    registry["last_updated"] = subprocess.run(
        ["date", "-u", "+%Y-%m-%dT%H:%M:%SZ"],
        capture_output=True, text=True, check=True
    ).stdout.strip()

    return registry


def create_registry_pr(entry: dict, dry_run: bool = False) -> str:
    """Create a PR to update the registry with the new entry."""
    ext_id = entry["id"]
    version = entry["version"]
    branch_name = f"update-{ext_id}-{version}"

    print(f"  Fetching current registry.json...")
    registry = fetch_registry_json()

    print(f"  Updating registry with {ext_id} v{version}...")
    updated_registry = update_registry(registry, entry)

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
         "-f", f"message=Update {ext_id} to v{version}",
         "-f", f"content={content_b64}",
         "-f", f"branch={branch_name}",
         "-f", f"sha={base_sha}"],
        capture_output=True,
        text=True,
        check=True,
    )

    # Create PR
    print(f"  Creating PR...")
    platform_summary = ", ".join(sorted(entry.get("platforms", {}).keys()))
    pr_body = (
        f"## Update `{ext_id}` to v{version}\n\n"
        f"**Platforms:** {platform_summary}\n\n"
        f"This PR updates the registry entry for `{ext_id}`.\n\n"
        f"---\n"
        f"*Generated by submit_to_registry.py*"
    )

    result = subprocess.run(
        ["gh", "pr", "create",
         "--repo", REGISTRY_REPO,
         "--head", branch_name,
         "--base", default_branch,
         "--title", f"Update {ext_id} to v{version}",
         "--body", pr_body],
        capture_output=True,
        text=True,
        check=True,
    )

    return result.stdout.strip()


def main():
    global SOURCE_REPO

    parser = argparse.ArgumentParser(
        description="Submit a plugin release to pj-plugin-registry.",
        epilog="Note: Release must already exist on GitHub. Use release_plugin.py to create it first.",
    )
    parser.add_argument(
        "plugin",
        help="Plugin directory (e.g. data_load_csv) or manifest id (e.g. csv-loader)",
    )
    parser.add_argument(
        "--version", "-v",
        metavar="VERSION",
        help="Version to submit (e.g. 1.0.5). If not specified, uses latest release from GitHub.",
    )
    parser.add_argument(
        "--list-releases", "-l",
        action="store_true",
        help="List available releases for the plugin and exit",
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
    args = parser.parse_args()

    SOURCE_REPO = args.releases_repo

    # Find plugin directory
    plugin_dir = find_plugin_dir(args.plugin)

    # Read manifest for artifact name
    manifest = read_manifest(plugin_dir)
    artifact_name = manifest["id"]

    print(f"Plugin: {plugin_dir}")
    print(f"Artifact: {artifact_name}")
    print(f"Source repo: {SOURCE_REPO}")

    # List releases if requested
    if args.list_releases:
        print(f"\nAvailable releases for {plugin_dir}:")
        releases = list_github_releases(plugin_dir)
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
        version = get_latest_release_version(plugin_dir)
        if not version:
            sys.exit(f"Error: No releases found for {plugin_dir} on {SOURCE_REPO}. Specify --version or create a release first.")
        print(f"  Latest version: {version}")

    tag = f"{plugin_dir}/v{version}"
    print(f"Tag: {tag}")

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
        assets, artifact_name, version,
        verify_checksums=not args.skip_checksum_verify
    )
    if not platforms:
        sys.exit("Error: No valid platform artifacts found")

    missing = set(PLATFORMS) - set(platforms.keys())
    if missing:
        print(f"  Warning: missing platforms: {', '.join(sorted(missing))}", file=sys.stderr)

    # Build registry entry
    entry = build_registry_entry(manifest, platforms)

    if args.dry_run:
        print(f"\nRegistry entry (dry-run):")
        print(json.dumps(entry, indent=2))

    # Create PR
    print(f"\nCreating PR on {REGISTRY_REPO}...")
    pr_url = create_registry_pr(entry, dry_run=args.dry_run)

    if not args.dry_run:
        print(f"\n✓ PR created: {pr_url}")


if __name__ == "__main__":
    main()
