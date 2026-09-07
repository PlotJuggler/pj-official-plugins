#!/usr/bin/env python3
"""Build a *local* registry.json from already-compiled plugins — no GitHub release.

This is the offline counterpart of ``submit_to_registry.py``. Instead of pulling
artifacts from a GitHub release and pointing the registry at github.com URLs, it
packages the plugin binaries sitting in a local build directory into ZIPs on disk
and writes a registry.json whose platform URLs are ``file://`` links to those
ZIPs. The result is directly loadable by the marketplace client's registry URL
(which accepts the ``file`` scheme), so you can exercise install/update/uninstall
end-to-end without cutting a release.

It reuses the packaging and registry-entry logic from ``release_tools.py`` and
``submit_to_registry.py`` verbatim, so a locally produced ZIP has the same layout
(``<extension_id>/`` at the archive root) as a real release artifact.

Platforms
---------
By default only the host platform is packaged (``--host-platform``, default
``linux-x86_64``) from ``--build-dir``. Other platforms — e.g. a Windows build
cross-mounted from another machine — are added with repeatable ``--platform``
flags:

    --platform windows-x86_64="$WINDOWS_BUILD_DIR"

A platform whose build directory is absent, or that has no matching binary, is
skipped with a warning rather than failing the whole run; the registry entry then
simply omits that platform. Coverage vs. the full platform set is reported at the
end so the gaps are visible.

Usage
-----
    # All plugins, host platform only, registry under build/local-registry/
    python3 scripts/build_local_registry.py

    # Specific plugins (source dir or extension id), custom output
    python3 scripts/build_local_registry.py csv-loader data_load_parquet \
        --output build/registry.json

    # Add a Windows x86_64 build from a cross-mounted directory
    python3 scripts/build_local_registry.py \
        --platform windows-x86_64="$WINDOWS_BUILD_DIR"
"""

import argparse
import shutil
import sys
import zipfile
from argparse import Namespace
from pathlib import Path

# The scripts directory must be importable so the shared release logic resolves.
SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))

from release_tools import (  # noqa: E402
    VALID_PLATFORMS,
    cmd_create_distribution_package,
    compute_sha256,
    find_source_dir,
    read_manifest,
    validate_manifest_file,
    validate_registry_entry,
)
from submit_to_registry import (  # noqa: E402
    build_registry_entry,
    update_registry,
)

# The top-level schema version the marketplace client and the registry validator
# expect. Kept in sync with pj-plugin-registry/registry.json.
REGISTRY_VERSION = "1.0"

# Repository root (the pj_ported_plugins checkout), where the plugin source
# directories with their manifest.json live.
REPO_ROOT = SCRIPT_DIR.parent


def platform_labels(platform: str) -> tuple[str, str]:
    """Split a registry platform key into (os_label, arch) for the ZIP filename.

    The ZIP name is cosmetic for a local registry — the entry maps the platform
    key to a URL regardless — so a plain split is enough (e.g. ``windows-x86_64``
    -> ``("windows", "x86_64")``).
    """
    os_label, _, arch = platform.partition("-")
    return os_label, arch


def package_platform(source_arg: str, extension_id: str, version: str, platform: str, build_dir: Path,
                     staging_root: Path, zip_dir: Path) -> Path | None:
    """Package one plugin for one platform into a ZIP on disk.

    Returns the path to the created ZIP, or ``None`` if the platform's build
    directory is missing or contains no matching binary (a skipped, non-fatal
    case — that platform is simply left out of the registry entry).
    """
    if not build_dir.is_dir():
        print(f"  - {platform}: build dir '{build_dir}' not found — skipping", file=sys.stderr)
        return None

    os_label, arch = platform_labels(platform)

    # cmd_create_distribution_package writes <output_dir>/<extension_id>/. Give
    # each (plugin, platform) its own output_dir so the later zip walk captures
    # exactly one extension tree, matching the release artifact layout.
    pkg_root = staging_root / platform / extension_id
    if pkg_root.exists():
        shutil.rmtree(pkg_root)
    pkg_root.mkdir(parents=True)

    pkg_args = Namespace(
        source=source_arg,
        release_tag=None,
        version=version,
        build_dir=build_dir,
        output_dir=pkg_root,
        os_label=os_label,
        arch=arch,
    )
    if cmd_create_distribution_package(pkg_args) != 0:
        print(f"  - {platform}: packaging failed (no binary for '{extension_id}'?) — skipping", file=sys.stderr)
        return None

    zip_dir.mkdir(parents=True, exist_ok=True)
    zip_path = zip_dir / f"{extension_id}-{version}-{os_label}-{arch}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for entry in sorted(pkg_root.rglob("*")):
            if entry.is_file():
                zf.write(entry, entry.relative_to(pkg_root))

    print(f"  - {platform}: {zip_path.name}", file=sys.stderr)
    return zip_path


def parse_platform_flag(value: str) -> tuple[str, Path]:
    """Parse a ``--platform NAME=BUILDDIR`` flag value."""
    name, sep, build_dir = value.partition("=")
    if not sep or not name or not build_dir:
        raise argparse.ArgumentTypeError(f"expected NAME=BUILDDIR, got '{value}'")
    if name not in VALID_PLATFORMS:
        raise argparse.ArgumentTypeError(f"invalid platform '{name}' (valid: {', '.join(VALID_PLATFORMS)})")
    return name, Path(build_dir)


def discover_plugins() -> list[str]:
    """Return every source directory name that has a manifest.json."""
    return sorted(p.parent.name for p in REPO_ROOT.glob("*/manifest.json"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build a local file:// registry.json from compiled plugins (no release).",
    )
    parser.add_argument(
        "plugins",
        nargs="*",
        help="Source directories or extension ids to include (default: all plugins with a manifest.json)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=REPO_ROOT / "build",
        help="Host build directory to package binaries from (default: build)",
    )
    parser.add_argument(
        "--host-platform",
        default="linux-x86_64",
        choices=VALID_PLATFORMS,
        help="Registry platform key for --build-dir (default: linux-x86_64)",
    )
    parser.add_argument(
        "--platform",
        dest="extra_platforms",
        action="append",
        default=[],
        type=parse_platform_flag,
        metavar="NAME=BUILDDIR",
        help="Additional platform to package from another build dir, as NAME=BUILDDIR (repeatable)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Path to write registry.json (default: <build-dir>/local-registry/registry.json)",
    )
    parser.add_argument(
        "--zip-dir",
        type=Path,
        help="Directory for the ZIP artifacts (default: next to registry.json, under artifacts/)",
    )
    parser.add_argument(
        "--keep-staging",
        action="store_true",
        help="Keep the intermediate staging tree instead of deleting it",
    )
    args = parser.parse_args()

    # Platform -> build dir. Host platform first; explicit --platform entries
    # override it if they name the same key.
    platforms: dict[str, Path] = {args.host_platform: args.build_dir}
    for name, build_dir in args.extra_platforms:
        platforms[name] = build_dir

    output_path = args.output or (args.build_dir / "local-registry" / "registry.json")
    output_path = output_path.resolve()
    zip_dir = (args.zip_dir or (output_path.parent / "artifacts")).resolve()
    staging_root = (output_path.parent / "staging").resolve()

    plugin_args = args.plugins or discover_plugins()
    if not plugin_args:
        print("Error: no plugins found (no */manifest.json under the repo root)", file=sys.stderr)
        return 1

    print(f"Repo root:   {REPO_ROOT}")
    print(f"Platforms:   {', '.join(f'{p} <- {d}' for p, d in platforms.items())}")
    print(f"Output:      {output_path}")
    print(f"ZIP dir:     {zip_dir}")
    print(f"Plugins:     {', '.join(plugin_args)}\n")

    registry: dict = {"registry_version": REGISTRY_VERSION, "extensions": []}
    covered_platforms: set[str] = set()
    failed: list[str] = []

    for source_arg in plugin_args:
        source_dir = find_source_dir(source_arg, REPO_ROOT)
        if source_dir is None:
            print(f"✗ {source_arg}: source directory not found — skipping", file=sys.stderr)
            failed.append(source_arg)
            continue

        manifest_path = source_dir / "manifest.json"
        manifest, errors = validate_manifest_file(manifest_path)
        if manifest is None or errors:
            print(f"✗ {source_arg}: invalid manifest", file=sys.stderr)
            for err in errors:
                print(f"    - {err}", file=sys.stderr)
            failed.append(source_arg)
            continue

        extension_id = manifest["id"]
        version = manifest["version"]
        print(f"• {extension_id} v{version} ({source_dir.name})")

        platform_entries: dict[str, dict] = {}
        for platform, build_dir in platforms.items():
            zip_path = package_platform(
                source_dir.name, extension_id, version, platform, build_dir, staging_root, zip_dir
            )
            if zip_path is None:
                continue
            platform_entries[platform] = {
                "url": zip_path.as_uri(),
                "checksum": f"sha256:{compute_sha256(zip_path)}",
            }
            covered_platforms.add(platform)

        if not platform_entries:
            print(f"✗ {extension_id}: no platform could be packaged — skipping\n", file=sys.stderr)
            failed.append(extension_id)
            continue

        entry = build_registry_entry(manifest, platform_entries)
        entry_errors = validate_registry_entry(entry)
        if entry_errors:
            print(f"  ! {extension_id}: registry entry has warnings:", file=sys.stderr)
            for err in entry_errors:
                print(f"      - {err}", file=sys.stderr)
        registry = update_registry(registry, entry)
        print()

    if not registry["extensions"]:
        print("Error: no extensions were packaged; registry not written", file=sys.stderr)
        return 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(_dump_registry(registry))

    if not args.keep_staging and staging_root.exists():
        shutil.rmtree(staging_root)

    print(f"✓ Wrote {len(registry['extensions'])} extension(s) to {output_path}")
    print(f"  Load it in the marketplace with registry URL: {output_path.as_uri()}")

    missing = sorted(set(VALID_PLATFORMS) - covered_platforms)
    if missing:
        print(f"  Platforms not covered (no build supplied): {', '.join(missing)}")
    if failed:
        print(f"  Plugins skipped: {', '.join(failed)}")
    return 0


def _dump_registry(registry: dict) -> str:
    """Serialize the registry with a trailing newline, matching the real file."""
    import json

    return json.dumps(registry, indent=2) + "\n"


if __name__ == "__main__":
    sys.exit(main())
