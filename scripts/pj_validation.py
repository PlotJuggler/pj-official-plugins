#!/usr/bin/env python3
"""
Centralized validation module for PlotJuggler plugin tooling.

This module provides reusable validation functions for:
- Version extraction and comparison (tag, manifest, binary)
- SHA256 checksum computation and verification
- URL accessibility checks
- Manifest validation
- Platform normalization
- Registry entry validation

Used by: release_plugin.py, submit_to_registry.py, verify_release.py, CI workflows
"""

import ctypes
import hashlib
import json
import re
import urllib.request
from pathlib import Path
from typing import Any


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
        # Additional fields exist but are not needed for version extraction
    ]


# =============================================================================
# VERSION FUNCTIONS
# =============================================================================


def extract_binary_version(plugin_path: Path) -> str | None:
    """
    Load a plugin binary and extract the embedded version from its manifest.

    Works on all platforms:
    - Linux: .so files
    - macOS: .dylib files
    - Windows: .dll files

    Args:
        plugin_path: Path to the plugin binary

    Returns:
        Version string if found, None if extraction failed
    """
    try:
        lib = ctypes.CDLL(str(plugin_path))
    except OSError:
        return None

    # Try both plugin types
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
        manifest = json.loads(manifest_str)
        return manifest.get("version")
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def extract_binary_manifest(plugin_path: Path) -> dict | None:
    """
    Load a plugin binary and extract the full embedded manifest.

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


def read_manifest_version(manifest_path: Path) -> str | None:
    """
    Read version from a manifest.json file.

    Args:
        manifest_path: Path to manifest.json

    Returns:
        Version string if found, None otherwise
    """
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
        return manifest.get("version")
    except (OSError, json.JSONDecodeError):
        return None


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


def validate_semver(version: str) -> bool:
    """
    Validate that a version string follows semantic versioning.

    Args:
        version: Version string to validate

    Returns:
        True if valid semver format, False otherwise
    """
    return SEMVER_REGEX.match(version) is not None


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


def verify_version_consistency(
    tag: str | None = None,
    manifest_path: Path | None = None,
    binary_path: Path | None = None,
) -> tuple[bool, dict[str, str | None], list[str]]:
    """
    Verify that all provided version sources match.

    Args:
        tag: Release tag (e.g., 'data_load_csv/v1.0.5')
        manifest_path: Path to manifest.json
        binary_path: Path to compiled plugin binary

    Returns:
        Tuple of:
        - bool: True if all versions match
        - dict: Version values found {'tag': ..., 'manifest': ..., 'binary': ...}
        - list: Error messages if any
    """
    versions: dict[str, str | None] = {}
    errors: list[str] = []

    # Extract tag version
    if tag:
        parsed = parse_tag_version(tag)
        if parsed:
            versions["tag"] = parsed[1]
        else:
            errors.append(f"Invalid tag format: {tag}")
            versions["tag"] = None
    else:
        versions["tag"] = None

    # Extract manifest version
    if manifest_path:
        versions["manifest"] = read_manifest_version(manifest_path)
        if versions["manifest"] is None:
            errors.append(f"Could not read manifest version: {manifest_path}")
    else:
        versions["manifest"] = None

    # Extract binary version
    if binary_path:
        versions["binary"] = extract_binary_version(binary_path)
        if versions["binary"] is None:
            errors.append(f"Could not extract binary version: {binary_path}")
    else:
        versions["binary"] = None

    # Compare all non-None versions
    non_null = {k: v for k, v in versions.items() if v is not None}
    if len(non_null) < 2:
        # Not enough versions to compare
        return len(errors) == 0, versions, errors

    unique_versions = set(non_null.values())
    if len(unique_versions) > 1:
        errors.append(f"Version mismatch: {non_null}")
        return False, versions, errors

    return True, versions, errors


# =============================================================================
# CHECKSUM FUNCTIONS
# =============================================================================


def compute_sha256(file_path: Path) -> str:
    """
    Compute SHA256 checksum of a file.

    Args:
        file_path: Path to file

    Returns:
        Hex-encoded SHA256 hash
    """
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(8192):
            sha256.update(chunk)
    return sha256.hexdigest()


def compute_sha256_bytes(data: bytes) -> str:
    """
    Compute SHA256 checksum of bytes.

    Args:
        data: Bytes to hash

    Returns:
        Hex-encoded SHA256 hash
    """
    return hashlib.sha256(data).hexdigest()


def verify_checksum(file_path: Path, expected: str) -> bool:
    """
    Verify SHA256 checksum of a file.

    Args:
        file_path: Path to file
        expected: Expected checksum (with or without 'sha256:' prefix)

    Returns:
        True if checksum matches
    """
    if expected.startswith("sha256:"):
        expected = expected[7:]

    actual = compute_sha256(file_path)
    return actual.lower() == expected.lower()


def verify_checksum_bytes(data: bytes, expected: str) -> bool:
    """
    Verify SHA256 checksum of bytes.

    Args:
        data: Bytes to verify
        expected: Expected checksum (with or without 'sha256:' prefix)

    Returns:
        True if checksum matches
    """
    if expected.startswith("sha256:"):
        expected = expected[7:]

    actual = compute_sha256_bytes(data)
    return actual.lower() == expected.lower()


# =============================================================================
# URL FUNCTIONS
# =============================================================================


def check_url_accessible(url: str, timeout: int = 10) -> tuple[bool, str]:
    """
    Check if a URL is accessible via HEAD request.

    Args:
        url: URL to check
        timeout: Request timeout in seconds

    Returns:
        Tuple of (success, error_message)
    """
    try:
        req = urllib.request.Request(url, method="HEAD")
        req.add_header("User-Agent", "pj-validation/1.0")
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
    """
    Download content from a URL.

    Args:
        url: URL to download
        timeout: Request timeout in seconds

    Returns:
        Tuple of (content_bytes, error_message)
    """
    try:
        req = urllib.request.Request(url)
        req.add_header("User-Agent", "pj-validation/1.0")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read(), ""
    except urllib.error.HTTPError as e:
        return None, f"HTTP {e.code}"
    except urllib.error.URLError as e:
        return None, str(e.reason)
    except Exception as e:
        return None, str(e)


def download_and_verify(url: str, expected_checksum: str, timeout: int = 60) -> tuple[bool, bytes | None, str]:
    """
    Download a file and verify its SHA256 checksum.

    Args:
        url: URL to download
        expected_checksum: Expected SHA256 (with or without 'sha256:' prefix)
        timeout: Request timeout in seconds

    Returns:
        Tuple of (success, content_bytes, error_message)
    """
    data, error = download_url(url, timeout)
    if data is None:
        return False, None, f"Download failed: {error}"

    if not verify_checksum_bytes(data, expected_checksum):
        actual = compute_sha256_bytes(data)
        expected = expected_checksum[7:] if expected_checksum.startswith("sha256:") else expected_checksum
        return False, data, f"Checksum mismatch: expected {expected[:16]}..., got {actual[:16]}..."

    return True, data, ""


# =============================================================================
# MANIFEST VALIDATION
# =============================================================================


def validate_manifest(manifest: dict) -> list[str]:
    """
    Validate a manifest dictionary.

    Args:
        manifest: Manifest dictionary

    Returns:
        List of error messages (empty if valid)
    """
    errors = []

    # Check required fields
    for field in REQUIRED_MANIFEST_FIELDS:
        if field not in manifest:
            errors.append(f"Missing required field: {field}")
        elif not manifest[field]:
            errors.append(f"Empty required field: {field}")

    # Validate version format
    if "version" in manifest and manifest["version"]:
        if not validate_semver(manifest["version"]):
            errors.append(f"Invalid version format: {manifest['version']} (expected semver)")

    # Validate category if present
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
# PLATFORM FUNCTIONS
# =============================================================================

# Platform normalization mappings
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

    Args:
        filename: ZIP filename

    Returns:
        Normalized platform string (e.g., 'linux-x86_64') or None if invalid
    """
    # Remove .zip extension
    if filename.endswith(".zip"):
        filename = filename[:-4]

    # Split by dash and try to find os-arch at the end
    parts = filename.split("-")
    if len(parts) < 4:
        return None

    # Last two parts should be arch and os (in reverse order from filename)
    arch_raw = parts[-1].lower()
    os_raw = parts[-2].lower()

    arch = PLATFORM_ARCH_MAP.get(arch_raw)
    os_name = PLATFORM_OS_MAP.get(os_raw)

    if arch and os_name:
        return f"{os_name}-{arch}"

    return None


def get_platform_extension(platform: str) -> str:
    """
    Get the library extension for a platform.

    Args:
        platform: Platform string (e.g., 'linux-x86_64')

    Returns:
        Library extension (e.g., 'so', 'dylib', 'dll')
    """
    if platform.startswith("linux"):
        return "so"
    elif platform.startswith("macos"):
        return "dylib"
    elif platform.startswith("windows"):
        return "dll"
    return "so"  # Default


# =============================================================================
# REGISTRY VALIDATION
# =============================================================================


def validate_registry_entry(entry: dict) -> list[str]:
    """
    Validate a registry entry dictionary.

    Args:
        entry: Registry entry dictionary

    Returns:
        List of error messages (empty if valid)
    """
    errors = []

    # Required fields
    required = ["id", "name", "version", "description", "author", "publisher", "license", "category", "platforms"]
    for field in required:
        if field not in entry:
            errors.append(f"Missing required field: {field}")

    # Validate category
    if entry.get("category") and entry["category"] not in VALID_CATEGORIES:
        errors.append(f"Invalid category: {entry['category']}")

    # Validate platforms
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
    """
    Convert kebab-case plugin ID to PascalCase class name.

    Args:
        plugin_id: Plugin ID (e.g., 'csv-loader')

    Returns:
        Class name (e.g., 'CsvLoader')
    """
    return "".join(word.capitalize() for word in plugin_id.replace("_", "-").split("-"))


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
        # Also search recursively
        binaries.extend(directory.glob(f"**/{pattern}.{ext}"))
    # Remove duplicates while preserving order
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


def find_all_plugin_binaries_with_manifest(directory: Path) -> list[tuple[Path, dict]]:
    """
    Find all plugin binaries and extract their embedded manifests.

    Args:
        directory: Directory to search (recursively)

    Returns:
        List of (binary_path, manifest_dict) tuples for binaries
        that have valid embedded manifests
    """
    results = []
    binaries = find_plugin_binaries(directory, "*")

    for binary_path in binaries:
        manifest = extract_binary_manifest(binary_path)
        if manifest:
            results.append((binary_path, manifest))

    return results
