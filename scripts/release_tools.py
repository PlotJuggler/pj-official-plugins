#!/usr/bin/env python3
"""
Release tools for PlotJuggler plugins.

This module provides utilities for the plugin release workflow:
- Version extraction from compiled binaries (via embedded manifest)
- Version consistency verification (tag vs manifest.json vs binary)
- Distribution package creation (locates binary by manifest id)
- Manifest validation

Used as library by: release_plugin.py, submit_to_registry.py
Used as CLI by: CI workflows

CLI Usage:
    # Verify that tag, manifest.json, and compiled binary have matching versions
    python3 scripts/release_tools.py verify-version-consistency data_load_csv \\
        --build-dir build/Release \\
        --expected-version 1.0.5

    # Create distribution package (finds binary by manifest id, copies to output dir)
    python3 scripts/release_tools.py create-distribution-package data_load_csv \\
        --build-dir build/Release \\
        --output-dir dist \\
        --os-label linux \\
        --arch x86_64

    # Extract and display the manifest embedded in a compiled binary
    python3 scripts/release_tools.py extract-embedded-manifest build/Release/libfoo.so

    # Validate manifest.json structure and required fields
    python3 scripts/release_tools.py validate-manifest data_load_csv/manifest.json
"""

import argparse
import ctypes
import hashlib
import json
import re
import shutil
import sys
import urllib.request
from pathlib import Path


# =============================================================================
# CONSTANTS
# =============================================================================

REQUIRED_MANIFEST_FIELDS = ["id", "name", "version"]
OPTIONAL_MANIFEST_FIELDS = ["description", "author", "publisher", "license", "category"]

VALID_PLATFORMS = [
    "linux-x86_64",
    "linux-arm64",
    "macos-x86_64",
    "macos-arm64",
    "windows-x86_64",
    "windows-arm64",
]

VALID_CATEGORIES = ["data_loader", "data_stream", "message_parser", "parser", "toolbox"]

# Semantic versioning regex (simplified: major.minor.patch with optional pre-release)
SEMVER_REGEX = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([a-zA-Z0-9.-]+))?(?:\+([a-zA-Z0-9.-]+))?$")


# =============================================================================
# CTYPES STRUCTURES FOR PLUGIN LOADING
# =============================================================================


class PluginVtable(ctypes.Structure):
    """
    Partial vtable structure matching PJ_data_source_vtable_t and PJ_message_parser_vtable_t.

    Both vtable types have the same initial fields up to manifest_json:
    - protocol_version (uint32_t) at offset +0
    - struct_size (uint32_t) at offset +4
    - create (void* function pointer) at offset +8
    - destroy (void* function pointer) at offset +16
    - manifest_json (const char*) at offset +24
    """

    _fields_ = [
        ("protocol_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("create", ctypes.c_void_p),
        ("destroy", ctypes.c_void_p),
        ("manifest_json", ctypes.c_char_p),
    ]


# =============================================================================
# BINARY MANIFEST EXTRACTION
# =============================================================================


def extract_binary_manifest(plugin_path: Path) -> dict | None:
    """
    Load a plugin binary and extract the full embedded manifest.

    Uses ctypes to load the shared library and call the vtable getter function.
    Works cross-platform: .so (Linux), .dylib (macOS), .dll (Windows).

    Args:
        plugin_path: Path to the plugin binary

    Returns:
        Manifest dict if found, None if extraction failed
    """
    try:
        lib = ctypes.CDLL(str(plugin_path))
    except OSError:
        return None

    vtable = None
    for func_name in ["PJ_get_data_source_vtable", "PJ_get_message_parser_vtable"]:
        try:
            get_vtable = getattr(lib, func_name)
            get_vtable.restype = ctypes.POINTER(PluginVtable)
            get_vtable.argtypes = []
            vtable = get_vtable()
            break
        except AttributeError:
            continue

    if not vtable or not vtable.contents.manifest_json:
        return None

    try:
        manifest_str = vtable.contents.manifest_json.decode("utf-8")
        return json.loads(manifest_str)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def extract_binary_version(plugin_path: Path) -> str | None:
    """
    Load a plugin binary and extract the version from its embedded manifest.

    Args:
        plugin_path: Path to the plugin binary

    Returns:
        Version string if found, None if extraction failed
    """
    manifest = extract_binary_manifest(plugin_path)
    if manifest:
        return manifest.get("version")
    return None


# =============================================================================
# MANIFEST FILE FUNCTIONS
# =============================================================================


def read_manifest(manifest_path: Path) -> dict | None:
    """
    Read and parse a manifest.json file.

    Args:
        manifest_path: Path to manifest.json

    Returns:
        Manifest dict if valid, None otherwise
    """
    try:
        with open(manifest_path) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def read_manifest_version(manifest_path: Path) -> str | None:
    """
    Read version from a manifest.json file.

    Args:
        manifest_path: Path to manifest.json

    Returns:
        Version string if found, None otherwise
    """
    manifest = read_manifest(manifest_path)
    if manifest:
        return manifest.get("version")
    return None


def validate_manifest(manifest: dict) -> list[str]:
    """
    Validate a manifest dictionary.

    Args:
        manifest: Manifest dictionary

    Returns:
        List of error messages (empty if valid)
    """
    errors = []

    for field in REQUIRED_MANIFEST_FIELDS:
        if field not in manifest:
            errors.append(f"Missing required field: {field}")
        elif not manifest[field]:
            errors.append(f"Empty required field: {field}")

    if "version" in manifest and manifest["version"]:
        if not validate_semver(manifest["version"]):
            errors.append(f"Invalid version format: {manifest['version']} (expected semver)")

    if "category" in manifest and manifest["category"]:
        if manifest["category"] not in VALID_CATEGORIES:
            errors.append(f"Invalid category: {manifest['category']} (valid: {VALID_CATEGORIES})")

    return errors


def validate_manifest_file(manifest_path: Path) -> tuple[dict | None, list[str]]:
    """
    Read and validate a manifest.json file.

    Args:
        manifest_path: Path to manifest.json

    Returns:
        Tuple of (manifest_dict, error_list)
    """
    if not manifest_path.exists():
        return None, [f"Manifest not found: {manifest_path}"]

    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except json.JSONDecodeError as e:
        return None, [f"Invalid JSON: {e}"]

    errors = validate_manifest(manifest)
    return manifest, errors


# =============================================================================
# VERSION FUNCTIONS
# =============================================================================


def validate_semver(version: str) -> bool:
    """
    Validate that a version string follows semantic versioning.

    Args:
        version: Version string to validate

    Returns:
        True if valid semver format, False otherwise
    """
    return SEMVER_REGEX.match(version) is not None


def parse_tag_version(tag: str) -> tuple[str, str] | None:
    """
    Parse a release tag into plugin directory and version.

    Expected format: 'plugin_dir/vX.Y.Z'

    Args:
        tag: Tag string (e.g., 'data_load_csv/v1.0.5')

    Returns:
        Tuple of (plugin_dir, version) or None if invalid format
    """
    match = re.match(r"^([^/]+)/v(.+)$", tag)
    if match:
        return match.group(1), match.group(2)
    return None


def compare_versions(v1: str, v2: str) -> int:
    """
    Compare two semantic version strings.

    Args:
        v1: First version
        v2: Second version

    Returns:
        -1 if v1 < v2, 0 if equal, 1 if v1 > v2
    """
    def parse(v: str) -> tuple:
        match = SEMVER_REGEX.match(v)
        if not match:
            return (0, 0, 0)
        return (int(match.group(1)), int(match.group(2)), int(match.group(3)))

    p1, p2 = parse(v1), parse(v2)
    if p1 < p2:
        return -1
    elif p1 > p2:
        return 1
    return 0


# =============================================================================
# BINARY DISCOVERY
# =============================================================================


def find_plugin_binaries(directory: Path, pattern: str = "*") -> list[Path]:
    """
    Find plugin binary files in a directory.

    Args:
        directory: Directory to search
        pattern: Glob pattern for filename (without extension)

    Returns:
        List of binary paths found
    """
    binaries = []
    for ext in ["so", "dylib", "dll"]:
        binaries.extend(directory.glob(f"{pattern}.{ext}"))
        binaries.extend(directory.glob(f"**/{pattern}.{ext}"))

    seen = set()
    unique = []
    for b in binaries:
        if b not in seen:
            seen.add(b)
            unique.append(b)
    return unique


def find_binary_by_manifest_id(directory: Path, manifest_id: str) -> Path | None:
    """
    Find a plugin binary by its embedded manifest id.

    Searches all plugin binaries in the directory (recursively),
    loads each one, extracts the embedded manifest, and returns
    the path to the binary whose manifest id matches.

    Args:
        directory: Directory to search (recursively)
        manifest_id: The manifest id to match (e.g., 'csv-loader')

    Returns:
        Path to matching binary, or None if not found
    """
    binaries = find_plugin_binaries(directory, "*")

    for binary_path in binaries:
        manifest = extract_binary_manifest(binary_path)
        if manifest and manifest.get("id") == manifest_id:
            return binary_path

    return None


# =============================================================================
# CHECKSUM FUNCTIONS
# =============================================================================


def compute_sha256(file_path: Path) -> str:
    """Compute SHA256 checksum of a file."""
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(8192):
            sha256.update(chunk)
    return sha256.hexdigest()


def compute_sha256_bytes(data: bytes) -> str:
    """Compute SHA256 checksum of bytes."""
    return hashlib.sha256(data).hexdigest()


def verify_checksum(file_path: Path, expected: str) -> bool:
    """Verify SHA256 checksum of a file."""
    if expected.startswith("sha256:"):
        expected = expected[7:]
    actual = compute_sha256(file_path)
    return actual.lower() == expected.lower()


def verify_checksum_bytes(data: bytes, expected: str) -> bool:
    """Verify SHA256 checksum of bytes."""
    if expected.startswith("sha256:"):
        expected = expected[7:]
    actual = compute_sha256_bytes(data)
    return actual.lower() == expected.lower()


# =============================================================================
# URL FUNCTIONS
# =============================================================================


def check_url_accessible(url: str, timeout: int = 10) -> tuple[bool, str]:
    """Check if a URL is accessible via HEAD request."""
    try:
        req = urllib.request.Request(url, method="HEAD")
        req.add_header("User-Agent", "release-tools/1.0")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            if resp.status == 200:
                return True, ""
            return False, f"HTTP {resp.status}"
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}"
    except urllib.error.URLError as e:
        return False, str(e.reason)
    except Exception as e:
        return False, str(e)


def download_url(url: str, timeout: int = 60) -> tuple[bytes | None, str]:
    """Download content from a URL."""
    try:
        req = urllib.request.Request(url)
        req.add_header("User-Agent", "release-tools/1.0")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read(), ""
    except urllib.error.HTTPError as e:
        return None, f"HTTP {e.code}"
    except urllib.error.URLError as e:
        return None, str(e.reason)
    except Exception as e:
        return None, str(e)


def download_and_verify(url: str, expected_checksum: str, timeout: int = 60) -> tuple[bool, bytes | None, str]:
    """Download a file and verify its SHA256 checksum."""
    data, error = download_url(url, timeout)
    if data is None:
        return False, None, f"Download failed: {error}"

    if not verify_checksum_bytes(data, expected_checksum):
        actual = compute_sha256_bytes(data)
        expected = expected_checksum[7:] if expected_checksum.startswith("sha256:") else expected_checksum
        return False, data, f"Checksum mismatch: expected {expected[:16]}..., got {actual[:16]}..."

    return True, data, ""


# =============================================================================
# PLATFORM FUNCTIONS
# =============================================================================

PLATFORM_ARCH_MAP = {
    "x64": "x86_64",
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "aarch64": "arm64",
    "arm64": "arm64",
}

PLATFORM_OS_MAP = {
    "linux": "linux",
    "macos": "macos",
    "darwin": "macos",
    "windows": "windows",
    "win": "windows",
}


def normalize_platform(filename: str) -> str | None:
    """
    Extract and normalize platform from a ZIP filename.

    Expected format: {artifact}-{version}-{os}-{arch}.zip
    """
    if filename.endswith(".zip"):
        filename = filename[:-4]

    parts = filename.split("-")
    if len(parts) < 4:
        return None

    arch_raw = parts[-1].lower()
    os_raw = parts[-2].lower()

    arch = PLATFORM_ARCH_MAP.get(arch_raw)
    os_name = PLATFORM_OS_MAP.get(os_raw)

    if arch and os_name:
        return f"{os_name}-{arch}"

    return None


def get_platform_extension(platform: str) -> str:
    """Get the library extension for a platform."""
    if platform.startswith("linux"):
        return "so"
    elif platform.startswith("macos"):
        return "dylib"
    elif platform.startswith("windows"):
        return "dll"
    return "so"


# =============================================================================
# REGISTRY VALIDATION
# =============================================================================


def validate_registry_entry(entry: dict) -> list[str]:
    """Validate a registry entry dictionary."""
    errors = []

    required = ["id", "name", "version", "description", "author", "publisher", "license", "category", "platforms"]
    for field in required:
        if field not in entry:
            errors.append(f"Missing required field: {field}")

    if entry.get("category") and entry["category"] not in VALID_CATEGORIES:
        errors.append(f"Invalid category: {entry['category']}")

    platforms = entry.get("platforms", {})
    if not isinstance(platforms, dict):
        errors.append("'platforms' must be an object")
    else:
        for platform, data in platforms.items():
            if platform not in VALID_PLATFORMS:
                errors.append(f"Invalid platform: {platform}")

            if not isinstance(data, dict):
                errors.append(f"{platform}: platform data must be an object")
                continue

            if "url" not in data:
                errors.append(f"{platform}: missing 'url'")

            if "checksum" not in data:
                errors.append(f"{platform}: missing 'checksum'")
            elif not data["checksum"].startswith("sha256:"):
                errors.append(f"{platform}: checksum must start with 'sha256:'")

    return errors


# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================


def id_to_class_name(plugin_id: str) -> str:
    """Convert kebab-case plugin ID to PascalCase class name."""
    return "".join(word.capitalize() for word in plugin_id.replace("_", "-").split("-"))


def find_plugin_dir(plugin_arg: str, root: Path) -> Path | None:
    """
    Find plugin directory by name or manifest id.

    Args:
        plugin_arg: Plugin directory name or manifest id
        root: Root directory to search from

    Returns:
        Path to plugin directory or None
    """
    direct = root / plugin_arg
    if direct.is_dir() and (direct / "manifest.json").exists():
        return direct

    for plugin_dir in root.iterdir():
        if not plugin_dir.is_dir():
            continue
        manifest_path = plugin_dir / "manifest.json"
        if not manifest_path.exists():
            continue
        manifest = read_manifest(manifest_path)
        if manifest and manifest.get("id") == plugin_arg:
            return plugin_dir

    return None


# =============================================================================
# CLI COMMANDS
# =============================================================================


def cmd_verify_version_consistency(args) -> int:
    """
    Verify that tag, manifest.json, and compiled binary all have matching versions.

    Used in CI after building to ensure the release is consistent.
    """
    script_dir = Path(__file__).parent
    root = script_dir.parent

    plugin_dir = find_plugin_dir(args.plugin, root)
    if not plugin_dir:
        print(f"Error: Plugin not found: {args.plugin}", file=sys.stderr)
        return 1

    manifest_path = plugin_dir / "manifest.json"
    manifest, errors = validate_manifest_file(manifest_path)
    if manifest is None:
        print(f"Error: Could not read manifest: {manifest_path}", file=sys.stderr)
        return 1

    if args.check_manifest and errors:
        print("Manifest validation errors:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    manifest_id = manifest.get("id")
    manifest_version = manifest.get("version")

    print(f"Plugin directory: {plugin_dir.name}")
    print(f"Manifest id:      {manifest_id}")
    print(f"Manifest version: {manifest_version}")

    versions = {"manifest": manifest_version}
    check_errors = []

    # Expected version (from tag)
    if args.expected_version:
        versions["expected"] = args.expected_version
        print(f"Expected version: {args.expected_version}")

    # Find and check binary
    if args.build_dir:
        print(f"\nSearching for binary with id '{manifest_id}' in {args.build_dir}...")
        binary_path = find_binary_by_manifest_id(args.build_dir, manifest_id)
        if binary_path:
            print(f"Found binary: {binary_path}")
            binary_version = extract_binary_version(binary_path)
            if binary_version:
                versions["binary"] = binary_version
                print(f"Binary version:   {binary_version}")
            else:
                check_errors.append(f"Could not extract version from binary: {binary_path}")
        else:
            check_errors.append(f"No binary found with manifest id '{manifest_id}' in {args.build_dir}")

    # Compare versions
    print()
    if len(versions) >= 2:
        unique = set(versions.values())
        if len(unique) == 1:
            print("OK: All versions match")
        else:
            print("ERROR: Version mismatch!", file=sys.stderr)
            for source, ver in versions.items():
                print(f"  {source}: {ver}", file=sys.stderr)
            check_errors.append("Version mismatch")

    # Validate semver
    for source, ver in versions.items():
        if ver and not validate_semver(ver):
            check_errors.append(f"Invalid semver format in {source}: {ver}")

    if check_errors:
        print(f"\nFAILED: {len(check_errors)} error(s)", file=sys.stderr)
        for err in check_errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print("\nPASSED: All checks successful")
    return 0


def cmd_create_distribution_package(args) -> int:
    """
    Create distribution package for a plugin.

    Finds the compiled binary by its embedded manifest id, copies it along
    with manifest.json to the output directory. Outputs the ZIP filename
    to stdout for CI to capture.
    """
    script_dir = Path(__file__).parent
    root = script_dir.parent

    plugin_dir = find_plugin_dir(args.plugin, root)
    if not plugin_dir:
        print(f"Error: Plugin not found: {args.plugin}", file=sys.stderr)
        return 1

    manifest_path = plugin_dir / "manifest.json"
    manifest = read_manifest(manifest_path)
    if not manifest:
        print(f"Error: Could not read manifest: {manifest_path}", file=sys.stderr)
        return 1

    artifact_id = manifest["id"]
    version = args.version or manifest["version"]

    # Find binary by manifest id
    print(f"Searching for binary with id '{artifact_id}' in {args.build_dir}...", file=sys.stderr)
    binary_path = find_binary_by_manifest_id(args.build_dir, artifact_id)
    if not binary_path:
        print(f"Error: No binary found with manifest id '{artifact_id}'", file=sys.stderr)
        return 1

    print(f"Found: {binary_path}", file=sys.stderr)

    # Create output directory
    output_path = args.output_dir / artifact_id
    output_path.mkdir(parents=True, exist_ok=True)

    # Copy binary
    shutil.copy(binary_path, output_path)
    print(f"Copied binary to: {output_path / binary_path.name}", file=sys.stderr)

    # Copy manifest
    shutil.copy(manifest_path, output_path)
    print(f"Copied manifest to: {output_path / 'manifest.json'}", file=sys.stderr)

    # Output ZIP filename to stdout
    if args.os_label and args.arch:
        zip_name = f"{artifact_id}-{version}-{args.os_label}-{args.arch}.zip"
        print(zip_name)
    else:
        print(artifact_id)

    return 0


def cmd_extract_embedded_manifest(args) -> int:
    """
    Extract and display the manifest embedded in a compiled plugin binary.

    Useful for debugging and verifying that the correct manifest was compiled in.
    """
    binary_path = Path(args.binary)
    if not binary_path.exists():
        print(f"Error: Binary not found: {binary_path}", file=sys.stderr)
        return 1

    manifest = extract_binary_manifest(binary_path)
    if manifest:
        print(json.dumps(manifest, indent=2))
        return 0
    else:
        print(f"Error: Could not extract manifest from: {binary_path}", file=sys.stderr)
        return 1


def cmd_validate_manifest(args) -> int:
    """
    Validate a manifest.json file structure and required fields.
    """
    manifest_path = Path(args.manifest)
    manifest, errors = validate_manifest_file(manifest_path)

    if manifest is None:
        print(f"Error: Could not read manifest", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    if errors:
        print(f"Validation errors in {manifest_path}:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print(f"OK: {manifest_path} is valid")
    print(f"  id:      {manifest.get('id')}")
    print(f"  name:    {manifest.get('name')}")
    print(f"  version: {manifest.get('version')}")
    return 0


# =============================================================================
# CLI MAIN
# =============================================================================


def main():
    parser = argparse.ArgumentParser(
        description="Release tools for PlotJuggler plugins",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    # verify-version-consistency
    p_verify = subparsers.add_parser(
        "verify-version-consistency",
        help="Verify that tag, manifest.json, and binary have matching versions",
        description="Compares versions from multiple sources to ensure release consistency. "
                    "Used in CI after building to catch version mismatches before publishing.",
    )
    p_verify.add_argument("plugin", help="Plugin directory name or manifest id")
    p_verify.add_argument("--build-dir", type=Path, help="Directory containing compiled binaries")
    p_verify.add_argument("--expected-version", help="Expected version (e.g., from git tag)")
    p_verify.add_argument("--check-manifest", action="store_true", help="Also validate manifest.json structure")
    p_verify.set_defaults(func=cmd_verify_version_consistency)

    # create-distribution-package
    p_package = subparsers.add_parser(
        "create-distribution-package",
        help="Create distribution package (binary + manifest) for a plugin",
        description="Finds the compiled binary by its embedded manifest id and copies it "
                    "along with manifest.json to the output directory. Outputs ZIP filename to stdout.",
    )
    p_package.add_argument("plugin", help="Plugin directory name or manifest id")
    p_package.add_argument("--build-dir", type=Path, required=True, help="Directory containing compiled binaries")
    p_package.add_argument("--output-dir", type=Path, required=True, help="Output directory for package")
    p_package.add_argument("--version", help="Version for ZIP filename (default: from manifest)")
    p_package.add_argument("--os-label", help="OS label for ZIP filename (linux, macos, windows)")
    p_package.add_argument("--arch", help="Architecture for ZIP filename (x86_64, arm64, etc.)")
    p_package.set_defaults(func=cmd_create_distribution_package)

    # extract-embedded-manifest
    p_extract = subparsers.add_parser(
        "extract-embedded-manifest",
        help="Extract and display the manifest embedded in a compiled binary",
        description="Loads a plugin binary via ctypes and extracts the JSON manifest "
                    "that was compiled into it. Useful for debugging version issues.",
    )
    p_extract.add_argument("binary", help="Path to compiled plugin binary (.so/.dll/.dylib)")
    p_extract.set_defaults(func=cmd_extract_embedded_manifest)

    # validate-manifest
    p_validate = subparsers.add_parser(
        "validate-manifest",
        help="Validate manifest.json structure and required fields",
        description="Checks that a manifest.json file has all required fields "
                    "and valid values (semver version, valid category, etc.).",
    )
    p_validate.add_argument("manifest", help="Path to manifest.json file")
    p_validate.set_defaults(func=cmd_validate_manifest)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
