#!/usr/bin/env python3
"""Create and push a release tag for an extension.

Terminology:
- extension: The distributable package (ZIP containing plugin binary + manifest.json)
- plugin: The compiled binary (.so/.dll/.dylib) containing the C++ class
- source_dir: The source directory containing code and manifest.json

Usage:
    python3 scripts/release_plugin.py data_load_csv
    python3 scripts/release_plugin.py csv-loader
    python3 scripts/release_plugin.py data_load_csv --dry-run

Prerequisites:
    - GitPython installed (pip install -r scripts/requirements.txt)
    - Push access to the target GitHub remote
    - Run from the repository root (pj_official_plugins)

After running this script, CI will build the extension artifacts.
Then use submit_to_registry.py to submit to the extension registry.
"""

import argparse
import os
import re
import sys
from pathlib import Path
from urllib.parse import urlparse

import git

# Add scripts directory to path for imports
SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))

from release_tools import (
    read_manifest,
    validate_manifest_file,
    validate_semver,
)

GITHUB_REMOTE_PATTERN = re.compile(r"github\.com[:/].+/pj-official-plugins")


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


def find_github_remote(repo: git.Repo) -> tuple[str, str] | None:
    """Find a remote pointing to a GitHub pj-official-plugins repo.

    Returns (remote_name, url) or None if not found.
    """
    for remote in repo.remotes:
        for url in remote.urls:
            if GITHUB_REMOTE_PATTERN.search(url):
                return remote.name, url
    return None


def check_tag_local(repo: git.Repo, tag: str) -> tuple[bool, str | None]:
    """Check if tag exists locally.

    Returns (exists, commit_sha).
    """
    if tag in [t.name for t in repo.tags]:
        return True, repo.tags[tag].commit.hexsha
    return False, None


def check_tag_remote(repo: git.Repo, remote_name: str, tag: str) -> tuple[bool, str | None]:
    """Check if tag exists on remote.

    Returns (exists, commit_sha).
    """
    try:
        refs = repo.git.ls_remote("--tags", remote_name, f"refs/tags/{tag}")
        if refs:
            commit = refs.split()[0]
            return True, commit
    except git.GitCommandError:
        pass
    return False, None


def build_authenticated_url(url: str, token: str) -> str | None:
    """Build authenticated URL for HTTPS remotes.

    Returns authenticated URL or None if not an HTTPS URL.
    """
    parsed = urlparse(url)
    if parsed.scheme != "https":
        return None
    # Build URL with token: https://token@github.com/owner/repo.git
    return f"https://{token}@{parsed.netloc}{parsed.path}"


def push_tag_with_auth(repo: git.Repo, remote_url: str, tag: str, token: str | None) -> None:
    """Push tag to remote, using token authentication if provided."""
    if token:
        auth_url = build_authenticated_url(remote_url, token)
        if auth_url:
            # Push directly to authenticated URL
            repo.git.push(auth_url, tag)
            return
        else:
            print(f"  Warning: Token provided but remote is not HTTPS, ignoring token")

    # Fall back to normal push (uses git credentials/SSH)
    repo.git.push(remote_url, tag)


def main():
    parser = argparse.ArgumentParser(
        description="Create and push a release tag for an extension.",
        epilog="After this, CI will build extension artifacts. Then use submit_to_registry.py to submit to registry.",
    )
    parser.add_argument(
        "source",
        help="Source directory (e.g. data_load_csv) or extension id (e.g. csv-loader)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be done without making changes",
    )
    parser.add_argument(
        "--remote",
        metavar="NAME",
        help="Git remote to push to (default: auto-detect GitHub remote)",
    )
    parser.add_argument(
        "--token",
        metavar="TOKEN",
        help="GitHub token for authentication (or set GITHUB_TOKEN env var)",
    )
    args = parser.parse_args()

    # Get token from args or environment
    github_token = args.token or os.environ.get("GITHUB_TOKEN")

    repo = git.Repo(".")
    head_commit = repo.head.commit.hexsha

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

    version = manifest["version"]
    extension_id = manifest["id"]
    tag = f"{source_dir}/v{version}"

    # Validate semver format
    if not validate_semver(version):
        sys.exit(f"Error: Invalid version format '{version}'. Expected semantic versioning (e.g., 1.0.0)")

    print(f"Source: {source_dir}")
    print(f"Version: {version}")
    print(f"Tag: {tag}")
    print(f"Extension: {extension_id}")
    print(f"HEAD: {head_commit[:12]}")

    # Find GitHub remote
    if args.remote:
        if args.remote not in [r.name for r in repo.remotes]:
            sys.exit(f"Error: Remote '{args.remote}' not found")
        remote_name = args.remote
        remote_url = list(repo.remotes[args.remote].urls)[0]
    else:
        result = find_github_remote(repo)
        if not result:
            print("\nError: No GitHub remote found for pj-official-plugins")
            print("Available remotes:")
            for r in repo.remotes:
                print(f"  - {r.name}: {list(r.urls)[0]}")
            print("\nUse --remote to specify one, or add a GitHub remote.")
            sys.exit(1)
        remote_name, remote_url = result

    print(f"\nRemote: {remote_name} ({remote_url})")

    # Check tag status
    print(f"\nChecking tag '{tag}'...")

    local_exists, local_commit = check_tag_local(repo, tag)
    remote_exists, remote_commit = check_tag_remote(repo, remote_name, tag)

    if local_exists:
        print(f"  Local:  exists at {local_commit[:12]}")
        if local_commit != head_commit:
            print(f"\n  ⚠ Warning: Tag points to different commit than HEAD!")
            print(f"    Tag commit:  {local_commit[:12]}")
            print(f"    HEAD commit: {head_commit[:12]}")
            print(f"\n  Did you forget to update the version in manifest.json?")
            print(f"  Current manifest version: {version}")
            sys.exit(1)
    else:
        print(f"  Local:  does not exist")

    if remote_exists:
        print(f"  Remote: exists at {remote_commit[:12]}")
        if remote_commit != head_commit:
            print(f"\n  ⚠ Warning: Remote tag points to different commit than HEAD!")
            print(f"    Remote commit: {remote_commit[:12]}")
            print(f"    HEAD commit:   {head_commit[:12]}")
            print(f"\n  Did you forget to update the version in manifest.json?")
            print(f"  Current manifest version: {version}")
            sys.exit(1)
    else:
        print(f"  Remote: does not exist")

    # Determine actions needed
    need_create_local = not local_exists
    need_push = not remote_exists

    if not need_create_local and not need_push:
        print(f"\n✓ Tag '{tag}' already exists locally and on remote at HEAD")
        print(f"  Nothing to do. CI should have created the release.")
        return

    # Create local tag if needed
    if need_create_local:
        print(f"\nCreating local tag '{tag}'...")
        if args.dry_run:
            print(f"  [dry-run] Would create tag at {head_commit[:12]}")
        else:
            repo.create_tag(tag)
            print(f"  ✓ Tag created at {head_commit[:12]}")

    # Push to remote if needed
    if need_push:
        print(f"\nPushing tag to {remote_name}...")
        if args.dry_run:
            print(f"  [dry-run] Would push tag to {remote_name}")
        else:
            try:
                push_tag_with_auth(repo, remote_url, tag, github_token)
                print(f"  ✓ Tag pushed to {remote_name}")
            except git.GitCommandError as e:
                print(f"  ✗ Error pushing tag: {e}")
                sys.exit(1)

    print(f"\n✓ Release tag '{tag}' created and pushed!")
    print(f"\nNext steps:")
    print(f"  1. Wait for CI to build extension artifacts")
    print(f"  2. Run: python3 scripts/submit_to_registry.py {args.source}")


if __name__ == "__main__":
    main()
