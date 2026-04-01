#!/usr/bin/env python3
"""Create and push a release tag for an extension.

Terminology:
- extension: The distributable package (ZIP containing plugin binary + manifest.json)
- plugin: The compiled binary (.so/.dll/.dylib) containing the C++ class
- source_dir: The source directory containing code and manifest.json

Usage:
    # Release with current manifest version
    python3 scripts/release_extension.py data_load_csv
    python3 scripts/release_extension.py csv-loader --submit-to-registry

    # Bump version automatically (updates manifest, commits, tags, pushes)
    python3 scripts/release_extension.py csv-loader --bump patch   # 1.0.5 -> 1.0.6
    python3 scripts/release_extension.py csv-loader --bump minor   # 1.0.5 -> 1.1.0
    python3 scripts/release_extension.py csv-loader --bump major   # 1.0.5 -> 2.0.0

    # Set explicit version
    python3 scripts/release_extension.py csv-loader --version 2.0.0

Prerequisites:
    - GitPython installed (pip install -r scripts/requirements.txt)
    - Push access to the target GitHub remote
    - Run from the repository root (pj_official_plugins)

The --submit-to-registry flag embeds metadata in the tag annotation that tells
CI to automatically create a PR to the extension registry after successful builds.
"""

import argparse
import json
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


def bump_version(current: str, bump_type: str) -> str:
    """Increment version according to semver.

    Args:
        current: Current version string (e.g., "1.0.5")
        bump_type: One of "major", "minor", "patch"

    Returns:
        New version string
    """
    parts = current.split(".")
    if len(parts) != 3:
        raise ValueError(f"Invalid version format: {current}")

    major, minor, patch = int(parts[0]), int(parts[1]), int(parts[2])

    if bump_type == "major":
        return f"{major + 1}.0.0"
    elif bump_type == "minor":
        return f"{major}.{minor + 1}.0"
    else:  # patch
        return f"{major}.{minor}.{patch + 1}"


def update_manifest_version(manifest_path: Path, new_version: str) -> None:
    """Update version in manifest.json.

    Preserves JSON formatting with 2-space indent.
    """
    manifest = json.loads(manifest_path.read_text())
    manifest["version"] = new_version
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")


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


def build_tag_message(extension_id: str, version: str, submit_to_registry: bool) -> str:
    """Build annotated tag message with JSON metadata.

    The metadata is used by CI to determine post-build actions.
    """
    metadata = {
        "extension_id": extension_id,
        "version": version,
        "auto_submit_to_registry": submit_to_registry,
    }
    return json.dumps(metadata, indent=2)


def main():
    parser = argparse.ArgumentParser(
        description="Create and push a release tag for an extension.",
        epilog="CI will build extension artifacts. Use --submit-to-registry for automatic PR creation.",
    )
    parser.add_argument(
        "source",
        help="Source directory (e.g. data_load_csv) or extension id (e.g. csv-loader)",
    )
    parser.add_argument(
        "--submit-to-registry",
        action="store_true",
        help="Automatically create registry PR after successful CI build",
    )
    parser.add_argument(
        "--bump",
        choices=["major", "minor", "patch"],
        metavar="TYPE",
        help="Bump version: major (1.0.0->2.0.0), minor (1.0.0->1.1.0), patch (1.0.0->1.0.1)",
    )
    parser.add_argument(
        "--version",
        metavar="VERSION",
        help="Set explicit version (e.g., 2.0.0). Updates manifest.json",
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
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force recreate tag even if it already exists. WARNING: This will invalidate "
             "existing registry checksums and break installations referencing the old release.",
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

    # Check for conflicting version arguments
    if args.bump and args.version:
        sys.exit("Error: Cannot use both --bump and --version. Choose one.")

    current_version = manifest["version"]
    extension_id = manifest["id"]

    # Handle version bump or explicit version
    if args.bump:
        try:
            version = bump_version(current_version, args.bump)
            print(f"Bumping version: {current_version} -> {version} ({args.bump})")
        except ValueError as e:
            sys.exit(f"Error: {e}")
    elif args.version:
        version = args.version
        if version == current_version:
            print(f"Warning: Specified version {version} is same as current version")
        else:
            print(f"Setting version: {current_version} -> {version}")
    else:
        version = current_version

    # Validate semver format
    if not validate_semver(version):
        sys.exit(f"Error: Invalid version format '{version}'. Expected semantic versioning (e.g., 1.0.0)")

    # Update manifest if version changed
    version_changed = version != current_version
    if version_changed:
        if args.dry_run:
            print(f"[dry-run] Would update {manifest_path} with version {version}")
        else:
            update_manifest_version(manifest_path, version)
            print(f"Updated {manifest_path}")

            # Commit the version change
            repo.index.add([str(manifest_path)])
            commit_msg = f"chore({source_dir}): bump version to {version}"
            repo.index.commit(commit_msg)
            print(f"Committed: {commit_msg}")
            # Update head_commit after the new commit
            head_commit = repo.head.commit.hexsha

    tag = f"{source_dir}/v{version}"

    print(f"Source: {source_dir}")
    print(f"Version: {version}")
    print(f"Tag: {tag}")
    print(f"Extension: {extension_id}")
    print(f"HEAD: {head_commit[:12]}")
    print(f"Auto-submit to registry: {'Yes' if args.submit_to_registry else 'No'}")

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
    else:
        print(f"  Local:  does not exist")

    if remote_exists:
        print(f"  Remote: exists at {remote_commit[:12]}")
    else:
        print(f"  Remote: does not exist")

    # Check for tag conflicts
    tag_conflict = False
    tag_at_head = False

    if local_exists and local_commit != head_commit:
        tag_conflict = True
        print(f"\n  Conflict: Local tag points to different commit than HEAD")
        print(f"    Tag commit:  {local_commit[:12]}")
        print(f"    HEAD commit: {head_commit[:12]}")

    if remote_exists and remote_commit != head_commit:
        tag_conflict = True
        print(f"\n  Conflict: Remote tag points to different commit than HEAD")
        print(f"    Remote commit: {remote_commit[:12]}")
        print(f"    HEAD commit:   {head_commit[:12]}")

    if local_exists and remote_exists and local_commit == head_commit and remote_commit == head_commit:
        tag_at_head = True

    # Handle conflicts
    if tag_conflict:
        if not args.force:
            print(f"\n  ERROR: Tag '{tag}' already exists at a different commit.")
            print(f"  Did you forget to update the version in manifest.json?")
            print(f"  Current manifest version: {version}")
            print(f"\n  To force recreate the tag, use: --force")
            print(f"  WARNING: Using --force will invalidate existing registry checksums!")
            sys.exit(1)
        else:
            print(f"\n  --force specified: Will delete and recreate tag")
            print(f"\n  *** WARNING ***")
            print(f"  Recreating an existing release tag will change the artifact checksums.")
            print(f"  If this version is already in the extension registry, existing installations")
            print(f"  will fail checksum verification and the registry entry must be updated.")
            print(f"  ***************")

            # Delete local tag if exists
            if local_exists:
                print(f"\n  Deleting local tag '{tag}'...")
                if not args.dry_run:
                    repo.delete_tag(tag)
                    print(f"    Deleted")
                else:
                    print(f"    [dry-run] Would delete")

            # Delete remote tag if exists
            if remote_exists:
                print(f"\n  Deleting remote tag '{tag}' from {remote_name}...")
                if not args.dry_run:
                    try:
                        repo.git.push(remote_url, "--delete", f"refs/tags/{tag}")
                        print(f"    Deleted")
                    except git.GitCommandError as e:
                        print(f"    Warning: Could not delete remote tag: {e}")
                else:
                    print(f"    [dry-run] Would delete")

            # Reset state after deletion
            local_exists = False
            remote_exists = False

    # Determine actions needed
    need_create_local = not local_exists
    need_push = not remote_exists

    if tag_at_head and not args.force:
        print(f"\n  Tag '{tag}' already exists locally and on remote at HEAD")
        print(f"  Nothing to do. CI should have created the release.")
        print(f"\n  To force recreate anyway, use: --force")
        return

    # Build tag message with metadata
    tag_message = build_tag_message(extension_id, version, args.submit_to_registry)

    # Create local tag if needed
    if need_create_local:
        print(f"\nCreating annotated tag '{tag}'...")
        if args.dry_run:
            print(f"  [dry-run] Would create tag at {head_commit[:12]}")
            print(f"  [dry-run] Tag message:\n{tag_message}")
        else:
            repo.create_tag(tag, message=tag_message)
            print(f"  Tag created at {head_commit[:12]}")

    # Push to remote if needed
    if need_push:
        print(f"\nPushing tag to {remote_name}...")
        if args.dry_run:
            print(f"  [dry-run] Would push tag to {remote_name}")
        else:
            try:
                push_tag_with_auth(repo, remote_url, tag, github_token)
                print(f"  Tag pushed to {remote_name}")
            except git.GitCommandError as e:
                print(f"  Error pushing tag: {e}")
                sys.exit(1)

    print(f"\n Release tag '{tag}' created and pushed!")
    if args.submit_to_registry:
        print(f"\nCI will automatically submit to registry after successful build.")
    else:
        print(f"\nNext steps:")
        print(f"  1. Wait for CI to build extension artifacts")
        print(f"  2. Run: python3 scripts/submit_to_registry.py {args.source}")


if __name__ == "__main__":
    main()
