#!/usr/bin/env python3
"""Create and push a release tag for an extension.

source_dir is the plugin source directory (code + manifest.json); the extension is the
distributable ZIP built from it.

Usage (from the repository root; needs GitPython and push access to the GitHub remote):
    python3 scripts/release_extension.py data_load_csv
    python3 scripts/release_extension.py csv-loader --bump patch|minor|major
    python3 scripts/release_extension.py csv-loader --version 2.0.0
    python3 scripts/release_extension.py csv-loader --submit-to-registry

--submit-to-registry records in the tag annotation that CI should open a registry PR
after a successful build.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path
from urllib.parse import urlparse

import git

SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))

from check_sdk_feature_floors import check_sdk_feature_floors
from release_tools import (
    read_manifest,
    validate_manifest_file,
    validate_semver,
)

GITHUB_REMOTE_PATTERN = re.compile(r"github\.com[:/].+/pj-official-plugins")


def bump_version(current: str, bump_type: str) -> str:
    """Bump a plain X.Y.Z version; any bump_type other than major/minor is a patch bump."""
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
    """Rewrite the manifest's version, re-serialized with a 2-space indent."""
    manifest = json.loads(manifest_path.read_text())
    manifest["version"] = new_version
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")


def find_source_dir(arg: str) -> str:
    """Resolve a directory name (data_load_csv) or manifest id (csv-loader) to a source directory."""
    if Path(arg).is_dir() and (Path(arg) / "manifest.json").exists():
        return arg

    for manifest_path in Path(".").glob("*/manifest.json"):
        manifest = read_manifest(manifest_path)
        if manifest and manifest.get("id") == arg:
            return manifest_path.parent.name

    sys.exit(f"Error: Source directory '{arg}' not found. Provide directory name (e.g. data_load_csv) or extension id (e.g. csv-loader)")


def find_github_remote(repo: git.Repo) -> tuple[str, str] | None:
    """Return (remote_name, url) of the first GitHub pj-official-plugins remote, or None."""
    for remote in repo.remotes:
        for url in remote.urls:
            if GITHUB_REMOTE_PATTERN.search(url):
                return remote.name, url
    return None


def check_tag_local(repo: git.Repo, tag: str) -> tuple[bool, str | None]:
    """Return (exists, commit_sha) for a local tag."""
    if tag in [t.name for t in repo.tags]:
        return True, repo.tags[tag].commit.hexsha
    return False, None


def check_tag_remote(repo: git.Repo, remote_name: str, tag: str) -> tuple[bool, str | None]:
    """Return (exists, commit_sha) for a remote tag."""
    try:
        refs = repo.git.ls_remote("--tags", remote_name, f"refs/tags/{tag}")
        if refs:
            commit = refs.split()[0]
            return True, commit
    except git.GitCommandError:
        pass
    return False, None


def build_authenticated_url(url: str, token: str) -> str | None:
    """Return the token-embedded URL for an HTTPS remote, None for any other scheme."""
    parsed = urlparse(url)
    if parsed.scheme != "https":
        return None
    return f"https://{token}@{parsed.netloc}{parsed.path}"


def push_tag_with_auth(repo: git.Repo, remote_url: str, tag: str, token: str | None) -> None:
    """Push tag to remote, using token authentication if provided."""
    if token:
        auth_url = build_authenticated_url(remote_url, token)
        if auth_url:
            repo.git.push(auth_url, tag)
            return
        else:
            print(f"  Warning: Token provided but remote is not HTTPS, ignoring token")

    repo.git.push(remote_url, tag)


def build_tag_message(extension_id: str, version: str, submit_to_registry: bool) -> str:
    """Tag message: JSON metadata that CI reads to decide post-build actions."""
    metadata = {
        "extension_id": extension_id,
        "version": version,
        "auto_submit_to_registry": submit_to_registry,
    }
    return json.dumps(metadata, indent=2)


def run_sdk_feature_floor_preflight(
    repo: "git.Repo",
    source_dir: str,
    *,
    out=sys.stdout,
    err=sys.stderr,
) -> bool:
    """Run the SDK-floor check, but only on inputs that match HEAD.

    The tag points at HEAD; scanning uncommitted files could validate a different tree
    from the one being tagged.
    """
    repo_root_text = repo.working_tree_dir
    if repo_root_text is None:
        print("Error: Release repository has no working tree", file=err)
        return False
    repo_root = Path(repo_root_text).resolve()
    source_path = Path(source_dir)
    if not source_path.is_absolute():
        source_path = repo_root / source_path
    try:
        source_pathspec = source_path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        print(f"Error: Plugin directory is outside the release repository: {source_dir}", file=err)
        return False

    checked_pathspecs = [source_pathspec, "common", "scripts", "SDK_VERSION"]
    dirty_output = repo.git.status(
        "--porcelain=v1",
        "--untracked-files=all",
        "--",
        *checked_pathspecs,
    )
    dirty_lines = [line for line in dirty_output.splitlines() if line]
    if dirty_lines:
        print(
            "Error: SDK feature-floor preflight inputs differ from HEAD; "
            "commit or stash them before creating a release tag:",
            file=err,
        )
        for line in dirty_lines:
            print(f"  {line}", file=err)
        return False

    print("Checking SDK feature-floor declaration...", file=out)
    return check_sdk_feature_floors(repo_root, [Path(source_pathspec)], out=out, err=err)


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

    github_token = args.token or os.environ.get("GITHUB_TOKEN")

    repo = git.Repo(".")
    head_commit = repo.head.commit.hexsha

    source_dir = find_source_dir(args.source)
    manifest_path = Path(source_dir) / "manifest.json"

    manifest, validation_errors = validate_manifest_file(manifest_path)
    if manifest is None:
        sys.exit(f"Error: Could not read {manifest_path}")
    if validation_errors:
        print(f"Error: Manifest validation failed:", file=sys.stderr)
        for err in validation_errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)

    if args.bump and args.version:
        sys.exit("Error: Cannot use both --bump and --version. Choose one.")

    current_version = manifest["version"]
    extension_id = manifest["id"]

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

    if not validate_semver(version):
        sys.exit(f"Error: Invalid version format '{version}'. Expected semantic versioning (e.g., 1.0.0)")

    version_changed = version != current_version
    if version_changed:
        if args.dry_run:
            print(f"[dry-run] Would update {manifest_path} with version {version}")
        else:
            update_manifest_version(manifest_path, version)
            print(f"Updated {manifest_path}")

            repo.index.add([str(manifest_path)])
            commit_msg = f"chore({source_dir}): bump version to {version}"
            repo.index.commit(commit_msg)
            print(f"Committed: {commit_msg}")
            head_commit = repo.head.commit.hexsha

    if not run_sdk_feature_floor_preflight(repo, source_dir):
        sys.exit(1)

    tag = f"{source_dir}/v{version}"

    print(f"Source: {source_dir}")
    print(f"Version: {version}")
    print(f"Tag: {tag}")
    print(f"Extension: {extension_id}")
    print(f"HEAD: {head_commit[:12]}")
    print(f"Submit to registry: {'Yes' if args.submit_to_registry else 'No'}")

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

            if local_exists:
                print(f"\n  Deleting local tag '{tag}'...")
                if not args.dry_run:
                    repo.delete_tag(tag)
                    print(f"    Deleted")
                else:
                    print(f"    [dry-run] Would delete")

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

            local_exists = False
            remote_exists = False

    need_create_local = not local_exists
    need_push = not remote_exists

    if tag_at_head and not args.force:
        print(f"\n  Tag '{tag}' already exists locally and on remote at HEAD")
        print(f"  Nothing to do. CI should have created the release.")
        print(f"\n  To force recreate anyway, use: --force")
        return

    tag_message = build_tag_message(extension_id, version, args.submit_to_registry)

    if need_create_local:
        print(f"\nCreating annotated tag '{tag}'...")
        if args.dry_run:
            print(f"  [dry-run] Would create tag at {head_commit[:12]}")
            print(f"  [dry-run] Tag message:\n{tag_message}")
        else:
            repo.create_tag(tag, message=tag_message)
            print(f"  Tag created at {head_commit[:12]}")

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
