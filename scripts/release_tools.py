#!/usr/bin/env python3
"""Release helpers for PlotJuggler extensions: manifest/version validation, packaging, release notes.

Imported by release_extension.py and submit_to_registry.py; the subcommands below are called by CI.
An extension is the ZIP (plugin binary + manifest.json) built from a source_dir.

Subcommands (see --help for options):
    verify-version-consistency <source> --build-dir D --expected-version V
    create-distribution-package <source> --build-dir D --output-dir O --os-label L --arch A
    extract-embedded-manifest <binary>
    validate-manifest <manifest.json>
    validate-distribution-package <zip> [--checksum-file F]
    generate-release-notes --release-tag <source_dir>/vX.Y.Z --output F
    resolve-build-scope --tag T --tag-type TT --os-label L --arch A
"""

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path


REQUIRED_MANIFEST_FIELDS = ["id", "name", "version", "category"]
# Free-text fields that must be strings when present. entry_point is where the user starts the
# extension in the host UI, as live label strings (e.g. "Toolbox menu → FFT"); it overrides the
# host's default derived from category/tags.
OPTIONAL_MANIFEST_FIELDS = ["description", "author", "publisher", "license", "entry_point"]

VALID_PLATFORMS = [
    "linux-x86_64",
    "linux-arm64",
    "macos-x86_64",
    "macos-arm64",
    "windows-x86_64",
    "windows-arm64",
]

VALID_CATEGORIES = ["data_loader", "data_stream", "message_parser", "toolbox", "widget"]

import sys as _sys

_sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
import feature_floor_check as floor_core  # noqa: E402

# SemVer 2.0.0 grammar (semver.org), the one the PlotJuggler host and the registry
# validator enforce; match it with fullmatch(). Groups: major, minor, patch, pre-release.
_SEMVER_IDENTIFIER = r"(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)"
SEMVER_REGEX = re.compile(
    rf"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    rf"(?:-({_SEMVER_IDENTIFIER}(?:\.{_SEMVER_IDENTIFIER})*))?"
    r"(?:\+[0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*)?",
    re.ASCII,
)
SDK_VERSION_REGEX = re.compile(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){2}(?:-dev\.(?:0|[1-9][0-9]*))?")


class PluginVtable(ctypes.Structure):
    """Common prefix of PJ_data_source/message_parser/toolbox/widget_vtable_t (64-bit offsets 0, 4, 8, 16, 24)."""

    _fields_ = [
        ("protocol_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("create", ctypes.c_void_p),
        ("destroy", ctypes.c_void_p),
        ("manifest_json", ctypes.c_char_p),
    ]


def extract_binary_manifest(plugin_path: Path) -> dict | None:
    """Load the plugin via ctypes and return its embedded manifest; None if it cannot be loaded or parsed."""
    # Widget-family DLLs depend on Qt. Since Python 3.8 Windows does not resolve a ctypes-loaded
    # DLL's dependencies through PATH, so point the loader at Qt's bin dir when the build exported it.
    if sys.platform == "win32" and os.environ.get("QT_ROOT_DIR"):
        qt_bin = Path(os.environ["QT_ROOT_DIR"]) / "bin"
        if qt_bin.is_dir():
            os.add_dll_directory(str(qt_bin))
    try:
        lib = ctypes.CDLL(str(plugin_path))
    except OSError:
        return None

    vtable = None
    for func_name in [
        "PJ_get_data_source_vtable",
        "PJ_get_message_parser_vtable",
        "PJ_get_toolbox_vtable",
        "PJ_get_widget_vtable",
    ]:
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
    manifest = extract_binary_manifest(plugin_path)
    if manifest:
        return manifest.get("version")
    return None


def read_manifest(manifest_path: Path) -> dict | None:
    """Parsed manifest, None if unreadable or not valid JSON."""
    try:
        with open(manifest_path) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def read_manifest_version(manifest_path: Path) -> str | None:
    manifest = read_manifest(manifest_path)
    if manifest:
        return manifest.get("version")
    return None


def validate_manifest(manifest: dict) -> list[str]:
    """Error messages for a manifest dict (empty if valid)."""
    if not isinstance(manifest, dict):
        return ["Manifest must be a JSON object"]

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

    for field in OPTIONAL_MANIFEST_FIELDS:
        if field in manifest and not isinstance(manifest[field], str):
            errors.append(f"Optional field {field} must be a string, got {type(manifest[field]).__name__}")

    return errors


def validate_sdk_minimum(manifest: dict, sdk_version: str) -> list[str]:
    """Check that min_sdk_required and SDK_VERSION name SDK releases (X.Y.Z or X.Y.Z-dev.N, with
    X.Y.Z-dev.N < X.Y.Z) and that the minimum does not exceed the build SDK. Declared values only:
    feature usage is not inferred.
    """
    minimum = manifest.get("min_sdk_required")
    for label, version in (("min_sdk_required", minimum), ("SDK_VERSION", sdk_version)):
        if not isinstance(version, str) or not SDK_VERSION_REGEX.fullmatch(version):
            return [f"{label} must name an SDK release (X.Y.Z or X.Y.Z-dev.N), got {version!r}"]
    if sdk_precedence(minimum) > sdk_precedence(sdk_version):
        return [f"min_sdk_required {minimum} exceeds build SDK_VERSION {sdk_version}"]
    return []


def sdk_precedence(version: str) -> tuple:
    """Sort key of a validated SDK version: a dev pre-release sorts before its release."""
    numbers, _, dev = version.partition("-dev.")
    return tuple(map(int, numbers.split("."))), 0 if dev else 1, int(dev or 0)


def validate_manifest_file(manifest_path: Path) -> tuple[dict | None, list[str]]:
    """Return (manifest or None if unreadable, errors)."""
    if not manifest_path.exists():
        return None, [f"Manifest not found: {manifest_path}"]

    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except json.JSONDecodeError as e:
        return None, [f"Invalid JSON: {e}"]

    errors = validate_manifest(manifest)
    errors.extend(floor_core.validate_manifest_shape(manifest))
    if not errors:
        sdk_version = (Path(__file__).resolve().parent.parent / "SDK_VERSION").read_text().strip()
        errors.extend(validate_sdk_minimum(manifest, sdk_version))
    return manifest, errors


def validate_semver(version: str) -> bool:
    """Whether `version` is SemVer 2.0.0: X.Y.Z[-pre-release][+build], no leading zeros.

    The registry and the host refuse anything else, so a release must fail here,
    before its tag exists.
    """
    return SEMVER_REGEX.fullmatch(version) is not None


def parse_tag_version(tag: str) -> tuple[str, str] | None:
    """Split 'source_dir/vX.Y.Z' (e.g. 'data_load_csv/v1.0.5') into (source_dir, version); None otherwise."""
    match = re.match(r"^([^/]+)/v(.+)$", tag)
    if match:
        return match.group(1), match.group(2)
    return None


def semver_sort_key(version: str) -> tuple:
    """Sort key with SemVer 2.0.0 precedence, the order the PlotJuggler host uses.

    A pre-release sorts before its release (X.Y.Z-dev.N < X.Y.Z), numeric
    pre-release identifiers compare as numbers and sort before alphanumeric
    ones, and build metadata is ignored. A string that is not a version sorts
    below every version.
    """
    match = SEMVER_REGEX.fullmatch(version)
    if not match:
        return ((0, 0, 0), 0, ())
    core = tuple(int(match.group(i)) for i in (1, 2, 3))
    if not match.group(4):
        return (core, 1, ())
    identifiers = tuple((0, int(part), "") if part.isdigit() else (1, 0, part) for part in match.group(4).split("."))
    return (core, 0, identifiers)


def find_plugin_binaries(directory: Path, pattern: str = "*") -> list[Path]:
    """De-duplicated .so/.dylib/.dll files matching pattern in directory and below."""
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
    """First binary under directory whose embedded manifest id matches (loads each candidate), else None."""
    matches = find_binaries_by_manifest_id(directory, manifest_id)
    return matches[0] if matches else None


def find_binaries_by_manifest_id(directory: Path, manifest_id: str) -> list[Path]:
    """All binaries with the given embedded manifest id. Some extensions ship several parser entry
    points under one id, and all of them must be packaged together.
    """
    binaries = find_plugin_binaries(directory, "*")
    matches = []

    for binary_path in binaries:
        manifest = extract_binary_manifest(binary_path)
        if manifest and manifest.get("id") == manifest_id:
            matches.append(binary_path)

    return sorted(matches)


def compute_sha256(file_path: Path) -> str:
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(8192):
            sha256.update(chunk)
    return sha256.hexdigest()


def compute_sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_checksum(file_path: Path, expected: str) -> bool:
    if expected.startswith("sha256:"):
        expected = expected[7:]
    actual = compute_sha256(file_path)
    return actual.lower() == expected.lower()


def verify_checksum_bytes(data: bytes, expected: str) -> bool:
    if expected.startswith("sha256:"):
        expected = expected[7:]
    actual = compute_sha256_bytes(data)
    return actual.lower() == expected.lower()


def check_url_accessible(url: str, timeout: int = 10) -> tuple[bool, str]:
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
    data, error = download_url(url, timeout)
    if data is None:
        return False, None, f"Download failed: {error}"

    if not verify_checksum_bytes(data, expected_checksum):
        actual = compute_sha256_bytes(data)
        expected = expected_checksum[7:] if expected_checksum.startswith("sha256:") else expected_checksum
        return False, data, f"Checksum mismatch: expected {expected[:16]}..., got {actual[:16]}..."

    return True, data, ""


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
    """'os-arch' platform of a {extension_id}-{version}-{os}-{arch}.zip name, None if unrecognized."""
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


def parse_zip_filename(filename: str) -> dict | None:
    """Split {extension_id}-{version}-{os}-{arch}.zip (e.g. csv-loader-1.0.5-linux-x86_64.zip) into a dict
    with extension_id, version, os_label, arch, platform; None if the name does not match.
    """
    basename = Path(filename).name
    if not basename.endswith(".zip"):
        return None

    name = basename[:-4]
    parts = name.split("-")

    if len(parts) < 4:
        return None

    arch_raw = parts[-1]
    os_raw = parts[-2]

    arch = PLATFORM_ARCH_MAP.get(arch_raw.lower())
    os_label = PLATFORM_OS_MAP.get(os_raw.lower())

    if not arch or not os_label:
        return None

    # The version precedes {os}-{arch}; the id (which may contain dashes) is everything before it.
    version_idx = len(parts) - 3
    if version_idx < 1:
        return None

    version = parts[version_idx]
    extension_id = "-".join(parts[:version_idx])

    if not extension_id or not validate_semver(version):
        return None

    return {
        "extension_id": extension_id,
        "version": version,
        "os_label": os_raw,
        "arch": arch_raw,
        "platform": f"{os_label}-{arch}",
    }


def get_platform_extension(platform: str) -> str:
    if platform.startswith("linux"):
        return "so"
    elif platform.startswith("macos"):
        return "dylib"
    elif platform.startswith("windows"):
        return "dll"
    return "so"


def validate_registry_entry(entry: dict) -> list[str]:
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


def id_to_class_name(extension_id: str) -> str:
    return "".join(word.capitalize() for word in extension_id.replace("_", "-").split("-"))


def find_source_dir(source_arg: str, root: Path) -> Path | None:
    """Source directory under root, matched by directory name or manifest id; None if not found."""
    direct = root / source_arg
    if direct.is_dir() and (direct / "manifest.json").exists():
        return direct

    for source_dir in root.iterdir():
        if not source_dir.is_dir():
            continue
        manifest_path = source_dir / "manifest.json"
        if not manifest_path.exists():
            continue
        manifest = read_manifest(manifest_path)
        if manifest and manifest.get("id") == source_arg:
            return source_dir

    return None


def run_git(root: Path, args: list[str]) -> str:
    result = subprocess.run(
        ["git"] + args,
        cwd=root,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def find_previous_release_tag(root: Path, source_name: str, current_tag: str) -> str | None:
    """Highest tag of the same plugin that is semver-older than current_tag, None if there is none."""
    parsed = parse_tag_version(current_tag)
    if not parsed:
        return None
    _, current_version = parsed

    tags = run_git(root, ["tag", "--list", f"{source_name}/v*"]).splitlines()
    candidates = []
    for tag in tags:
        tag = tag.strip()
        if not tag or tag == current_tag:
            continue
        tag_info = parse_tag_version(tag)
        if not tag_info:
            continue
        _, version = tag_info
        if semver_sort_key(version) < semver_sort_key(current_version):
            candidates.append((version, tag))

    if not candidates:
        return None

    candidates.sort(key=lambda item: semver_sort_key(item[0]))
    return candidates[-1][1]


def list_commits_for_paths(root: Path, revision_range: str, paths: list[str]) -> list[tuple[str, str]]:
    """(hash, subject) of commits in revision_range touching paths, oldest first, without merges or version bumps."""
    args = ["log", "--reverse", "--no-merges", "--format=%H%x1f%s", revision_range]
    if paths:
        args += ["--"] + paths
    output = run_git(root, args)
    commits = []
    for line in output.splitlines():
        if not line:
            continue
        commit_hash, _, subject = line.partition("\x1f")
        if commit_hash and subject and not is_release_bookkeeping(subject):
            commits.append((commit_hash, subject))
    return commits


def is_release_bookkeeping(subject: str) -> bool:
    """Version-bump commits, kept out of release notes."""
    normalized = subject.lower()
    return (
        normalized.startswith("chore(release):")
        or "bump version to" in normalized
        or "bump all plugins" in normalized
    )


def format_commit_list(commits: list[tuple[str, str]]) -> str:
    if not commits:
        return "- No plugin-specific changes detected in this range."
    return "\n".join(f"- {subject} ({commit_hash[:7]})" for commit_hash, subject in commits)


def cmd_verify_version_consistency(args) -> int:
    """Fail unless the tag, manifest.json and built plugin binary report the same version (CI, after the build)."""
    script_dir = Path(__file__).parent
    root = script_dir.parent

    if args.release_tag:
        parsed = parse_tag_version(args.release_tag)
        if not parsed:
            print(f"Error: Invalid release tag format: {args.release_tag}", file=sys.stderr)
            print(f"Expected: {{source_dir}}/v{{version}} (e.g., data_load_csv/v1.0.5)", file=sys.stderr)
            return 1
        source_name, expected_version = parsed
        if not args.expected_version:
            args.expected_version = expected_version
    elif args.source:
        source_name = args.source
    else:
        print("Error: Either source or --release-tag must be provided", file=sys.stderr)
        return 1

    source_dir = find_source_dir(source_name, root)
    if not source_dir:
        print(f"Error: Source directory not found: {source_name}", file=sys.stderr)
        return 1

    manifest_path = source_dir / "manifest.json"
    manifest, errors = validate_manifest_file(manifest_path)
    if manifest is None:
        print(f"Error: Could not read manifest: {manifest_path}", file=sys.stderr)
        return 1

    if args.check_manifest and errors:
        print("Manifest validation errors:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    extension_id = manifest.get("id")
    manifest_version = manifest.get("version")

    print(f"Source directory: {source_dir.name}")
    print(f"Extension id:     {extension_id}")
    print(f"Manifest version: {manifest_version}")

    versions = {"manifest": manifest_version}
    check_errors = []

    if args.expected_version:
        versions["expected"] = args.expected_version
        print(f"Expected version: {args.expected_version}")

    if args.build_dir:
        # A <build_dir>/dist/ staging tree (from a release.sh override) holds a binary that either proxies to
        # a per-platform inner (data_stream_ros2 loads it only when ROS is available) or links host-only deps
        # ctypes cannot load on a CI runner, so the embedded-manifest check is skipped and the staged
        # manifest.json is compared instead.
        staging_dir = args.build_dir / "dist"
        if staging_dir.is_dir() and any(staging_dir.iterdir()):
            print(f"\nStaging tree detected at {staging_dir} — skipping embedded-manifest binary check")
            staged_manifest = staging_dir / "manifest.json"
            if staged_manifest.is_file():
                staged_version = read_manifest_version(staged_manifest)
                if staged_version:
                    versions["staged_manifest"] = staged_version
                    print(f"Staged manifest version: {staged_version}")
        else:
            print(f"\nSearching for plugin binary with id '{extension_id}' in {args.build_dir}...")
            all_binaries = find_plugin_binaries(args.build_dir, "*")
            if all_binaries:
                print(f"Found {len(all_binaries)} binary file(s):")
                for b in all_binaries[:10]:
                    print(f"  - {b}")
                if len(all_binaries) > 10:
                    print(f"  ... and {len(all_binaries) - 10} more")
            else:
                print("  No binary files found in directory")
            binary_path = find_binary_by_manifest_id(args.build_dir, extension_id)
            if binary_path:
                print(f"Found plugin: {binary_path}")
                binary_version = extract_binary_version(binary_path)
                if binary_version:
                    versions["plugin"] = binary_version
                    print(f"Plugin version:   {binary_version}")
                else:
                    check_errors.append(f"Could not extract version from plugin: {binary_path}")
            else:
                if all_binaries:
                    print(f"\nManifest IDs found in binaries:")
                    for b in all_binaries:
                        manifest = extract_binary_manifest(b)
                        if manifest:
                            print(f"  - {b.name}: id='{manifest.get('id', 'N/A')}'")
                        else:
                            print(f"  - {b.name}: (no manifest)")
                check_errors.append(f"No plugin found with extension id '{extension_id}' in {args.build_dir}")

    for source, ver in versions.items():
        if ver and not validate_semver(ver):
            check_errors.append(f"Invalid semver format in {source}: {ver}")

    print()
    if len(versions) >= 2:
        unique = set(versions.values())
        if len(unique) == 1:
            print("Versions: all match")
        else:
            print("ERROR: Version mismatch!", file=sys.stderr)
            for source, ver in versions.items():
                print(f"  {source}: {ver}", file=sys.stderr)
            check_errors.append("Version mismatch")

    if check_errors:
        print(f"\nFAILED: {len(check_errors)} error(s)", file=sys.stderr)
        for err in check_errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print("\nPASSED: All checks successful")
    return 0


def cmd_create_distribution_package(args) -> int:
    """Assemble the extension tree in <output_dir>/<extension_id>/ and print the ZIP filename for CI.

    Staging mode (<build_dir>/dist/ exists and is non-empty): the plugin's release.sh pre-assembled a
    multi-artifact layout (e.g. proxy at the root + per-distro binaries under dist/<distro>/); it is copied
    as-is, plus manifest.json if the tree lacks one.
    Heuristic mode: every binary whose embedded manifest id matches, plus manifest.json and any python3*
    directory next to a binary.
    """
    script_dir = Path(__file__).parent
    root = script_dir.parent

    if args.release_tag:
        parsed = parse_tag_version(args.release_tag)
        if not parsed:
            print(f"Error: Invalid release tag format: {args.release_tag}", file=sys.stderr)
            print(f"Expected: {{source_dir}}/v{{version}} (e.g., data_load_csv/v1.0.5)", file=sys.stderr)
            return 1
        source_name, tag_version = parsed
        if not args.version:
            args.version = tag_version
    elif args.source:
        source_name = args.source
    else:
        print("Error: Either source or --release-tag must be provided", file=sys.stderr)
        return 1

    source_dir = find_source_dir(source_name, root)
    if not source_dir:
        print(f"Error: Source directory not found: {source_name}", file=sys.stderr)
        return 1

    manifest_path = source_dir / "manifest.json"
    manifest = read_manifest(manifest_path)
    if not manifest:
        print(f"Error: Could not read manifest: {manifest_path}", file=sys.stderr)
        return 1

    extension_id = manifest["id"]
    version = args.version or manifest["version"]

    output_path = args.output_dir / extension_id
    output_path.mkdir(parents=True, exist_ok=True)

    staging_dir = args.build_dir / "dist"
    if staging_dir.is_dir() and any(staging_dir.iterdir()):
        print(f"Staging tree found at {staging_dir} — packaging as-is", file=sys.stderr)
        for entry in sorted(staging_dir.iterdir()):
            dest = output_path / entry.name
            if entry.is_dir():
                shutil.copytree(entry, dest, dirs_exist_ok=True)
            else:
                shutil.copy(entry, dest)
            print(f"  staged {entry.name}", file=sys.stderr)

        # Never ship without a manifest.json, even if the staging tree omits it.
        if not (output_path / "manifest.json").is_file():
            shutil.copy(manifest_path, output_path)
            print(f"  added missing manifest.json from {manifest_path}", file=sys.stderr)
    else:
        print(f"Searching for plugin(s) with id '{extension_id}' in {args.build_dir}...", file=sys.stderr)
        plugin_paths = find_binaries_by_manifest_id(args.build_dir, extension_id)
        if not plugin_paths:
            print(f"Error: No plugin found with extension id '{extension_id}'", file=sys.stderr)
            return 1

        print(f"Found {len(plugin_paths)} plugin binary file(s):", file=sys.stderr)
        for plugin_path in plugin_paths:
            print(f"  - {plugin_path}", file=sys.stderr)
            shutil.copy(plugin_path, output_path)
            print(f"Copied plugin to: {output_path / plugin_path.name}", file=sys.stderr)

        shutil.copy(manifest_path, output_path)
        print(f"Copied manifest to: {output_path / 'manifest.json'}", file=sys.stderr)

        # Python stdlib directories placed next to the binary by a CMake POST_BUILD step (e.g. python3.12/)
        # make the plugin self-contained: no system Python is needed at runtime.
        bundled_python_dirs = set()
        for plugin_path in plugin_paths:
            for entry in plugin_path.parent.iterdir():
                if entry.is_dir() and entry.name.startswith("python3") and entry not in bundled_python_dirs:
                    bundled_python_dirs.add(entry)
                    dest = output_path / entry.name
                    shutil.copytree(entry, dest, dirs_exist_ok=True)
                    print(f"Bundled Python stdlib: {dest}", file=sys.stderr)

    if args.os_label and args.arch:
        zip_name = f"{extension_id}-{version}-{args.os_label}-{args.arch}.zip"
        print(zip_name)
    else:
        print(extension_id)

    return 0


def cmd_extract_embedded_manifest(args) -> int:
    plugin_path = Path(args.plugin)
    if not plugin_path.exists():
        print(f"Error: Plugin not found: {plugin_path}", file=sys.stderr)
        return 1

    manifest = extract_binary_manifest(plugin_path)
    if manifest:
        print(json.dumps(manifest, indent=2))
        return 0
    else:
        print(f"Error: Could not extract manifest from plugin: {plugin_path}", file=sys.stderr)
        return 1


def cmd_validate_manifest(args) -> int:
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


def cmd_validate_distribution_package(args) -> int:
    """Check a distribution ZIP: filename format, SHA256 (with --checksum-file), a plugin binary and
    manifest.json present, embedded manifest id/version equal to manifest.json, and filename id/version equal
    to the manifest's.
    """
    zip_path = Path(args.package)
    if not zip_path.exists():
        print(f"Error: Package not found: {zip_path}", file=sys.stderr)
        return 1

    errors = []
    print(f"Validating: {zip_path.name}")
    print()

    print("Filename parsing:")
    filename_info = parse_zip_filename(zip_path.name)
    if filename_info:
        print(f"  Extension: {filename_info['extension_id']}")
        print(f"  Version:   {filename_info['version']}")
        print(f"  Platform:  {filename_info['platform']}")
        print("  OK: Filename format valid")
    else:
        print("  ERROR: Invalid filename format")
        print("  Expected: {extension_id}-{version}-{os}-{arch}.zip")
        errors.append("Invalid filename format")
    print()

    if args.checksum_file:
        print("Checksum verification:")
        checksum_path = Path(args.checksum_file)
        if not checksum_path.exists():
            print(f"  ERROR: Checksum file not found: {checksum_path}")
            errors.append("Checksum file not found")
        else:
            with open(checksum_path) as f:
                content = f.read().strip()
            # sha256sum output ("hash  filename") or a bare hash
            expected_hash = content.split()[0]
            actual_hash = compute_sha256(zip_path)
            print(f"  Expected: {expected_hash[:16]}...")
            print(f"  Actual:   {actual_hash[:16]}...")
            if actual_hash.lower() == expected_hash.lower():
                print("  OK: SHA256 matches")
            else:
                print("  ERROR: SHA256 mismatch")
                errors.append("SHA256 checksum mismatch")
        print()

    print("Extension contents:")
    try:
        with zipfile.ZipFile(zip_path, 'r') as zf:
            names = zf.namelist()

            plugin_name = None
            manifest_name = None
            for name in names:
                if name.endswith('.so') or name.endswith('.dll') or name.endswith('.dylib'):
                    plugin_name = name
                if name.endswith('manifest.json'):
                    manifest_name = name

            if plugin_name:
                print(f"  Plugin:   {plugin_name}")
            else:
                print("  ERROR: No plugin found (.so/.dll/.dylib)")
                errors.append("No plugin in extension")

            if manifest_name:
                print(f"  Manifest: {manifest_name}")
            else:
                print("  ERROR: No manifest.json found")
                errors.append("No manifest.json in extension")

            print()

            if plugin_name and manifest_name:
                print("Manifest consistency:")

                with tempfile.TemporaryDirectory() as tmpdir:
                    zf.extractall(tmpdir)
                    tmp_path = Path(tmpdir)

                    file_manifest_path = tmp_path / manifest_name
                    file_manifest = read_manifest(file_manifest_path)

                    plugin_path = tmp_path / plugin_name
                    plugin_manifest = extract_binary_manifest(plugin_path)

                    if file_manifest:
                        print(f"  File manifest:   id={file_manifest.get('id')}, version={file_manifest.get('version')}")
                    else:
                        print("  ERROR: Could not read manifest.json")
                        errors.append("Invalid manifest.json in extension")

                    if plugin_manifest:
                        print(f"  Plugin manifest: id={plugin_manifest.get('id')}, version={plugin_manifest.get('version')}")
                    else:
                        print("  ERROR: Could not extract manifest from plugin")
                        errors.append("Plugin has no embedded manifest")

                    if file_manifest and plugin_manifest:
                        if (file_manifest.get('id') == plugin_manifest.get('id') and
                            file_manifest.get('version') == plugin_manifest.get('version')):
                            print("  OK: Manifests match")
                        else:
                            print("  ERROR: Manifests do not match")
                            errors.append("File manifest != plugin manifest")

                    print()

                    if filename_info and file_manifest:
                        print("Filename vs content:")
                        print(f"  Filename version:  {filename_info['version']}")
                        print(f"  Manifest version:  {file_manifest.get('version')}")
                        print(f"  Filename extension id: {filename_info['extension_id']}")
                        print(f"  Manifest id:           {file_manifest.get('id')}")

                        version_match = filename_info['version'] == file_manifest.get('version')
                        id_match = filename_info['extension_id'] == file_manifest.get('id')

                        if version_match and id_match:
                            print("  OK: Filename matches content")
                        else:
                            if not version_match:
                                print("  ERROR: Version mismatch")
                                errors.append("Filename version != manifest version")
                            if not id_match:
                                print("  ERROR: Extension ID mismatch")
                                errors.append("Filename extension id != manifest id")
                        print()

    except zipfile.BadZipFile:
        print("  ERROR: Invalid ZIP file")
        errors.append("Invalid ZIP file")
        print()

    if errors:
        print(f"FAILED: {len(errors)} error(s)")
        for err in errors:
            print(f"  - {err}")
        return 1
    else:
        print("PASSED: All validations successful")
        return 0


def cmd_generate_release_notes(args) -> int:
    """Release notes limited to commits that touched the plugin's directory (GitHub's generated notes are
    repository-wide); --include-shared lists shared build/runtime commits in a separate section.
    """
    script_dir = Path(__file__).parent
    root = script_dir.parent

    parsed = parse_tag_version(args.release_tag)
    if not parsed:
        print(f"Error: Invalid release tag format: {args.release_tag}", file=sys.stderr)
        print(f"Expected: {{source_dir}}/v{{version}} (e.g., data_load_csv/v1.0.5)", file=sys.stderr)
        return 1

    source_name, version = parsed
    source_dir = find_source_dir(source_name, root)
    if not source_dir:
        print(f"Error: Source directory not found: {source_name}", file=sys.stderr)
        return 1

    manifest_path = source_dir / "manifest.json"
    manifest = read_manifest(manifest_path)
    if not manifest:
        print(f"Error: Could not read manifest: {manifest_path}", file=sys.stderr)
        return 1

    previous_tag = args.previous_tag or find_previous_release_tag(root, source_name, args.release_tag)
    revision_range = f"{previous_tag}..{args.release_tag}" if previous_tag else args.release_tag
    plugin_commits = list_commits_for_paths(root, revision_range, [source_name])

    shared_commits = []
    if args.include_shared:
        shared_paths = [
            ".github/workflows/build-release.yml",
            "CMakeLists.txt",
            "cmake",
            "conanfile.py",
            "SDK_VERSION",
            "scripts",
        ]
        shared_commits = list_commits_for_paths(root, revision_range, shared_paths)
        plugin_hashes = {commit_hash for commit_hash, _ in plugin_commits}
        shared_commits = [(commit_hash, subject) for commit_hash, subject in shared_commits
                          if commit_hash not in plugin_hashes]

    title = f"{manifest.get('name', manifest.get('id', source_name))} v{version}"
    if previous_tag:
        intro = f"Changes since `{previous_tag}`."
    else:
        intro = "First tagged release for this plugin."

    sections = [
        f"## {title}",
        "",
        intro,
        "",
        "### Plugin Changes",
        "",
        format_commit_list(plugin_commits),
    ]

    if shared_commits:
        sections += [
            "",
            "### Shared Build And Runtime Changes",
            "",
            format_commit_list(shared_commits),
        ]

    sections += [
        "",
        "### Artifacts",
        "",
        "Platform-specific ZIP packages and SHA256 checksum files are attached to this release.",
        "",
    ]

    notes = "\n".join(sections)
    if args.output:
        args.output.write_text(notes)
    else:
        print(notes)

    return 0


def cmd_resolve_build_scope(args) -> int:
    """Resolve a CI matrix entry's build scope and print it as key=value lines for $GITHUB_OUTPUT.

    Outputs: build_dir, build_script_args, conan_hash, scope, plugin_name, platform, skip,
    use_release_override, needs_qt. Notices and errors go to stderr, where Actions parses
    `::notice::` / `::error::`.

    Per-plugin inputs honoured for plugin tags:
      supported_platforms  manifest list of canonical "os-arch" strings (VALID_PLATFORMS); other matrix
                           entries are skipped.
      <plugin>/release.sh  if executable, replaces the default build flow and is responsible for build and tests.
      category "widget"    links Qt, so needs_qt=true makes the workflow install the official Qt on that leg
                           only. The host loads a widget library only when built against the same Qt
                           major.minor, so a widget manifest limits supported_platforms to where CI installs
                           Qt (linux-x86_64 and windows-x86_64).
    """
    canonical_arch = PLATFORM_ARCH_MAP.get(args.arch.lower(), args.arch)
    canonical_os = PLATFORM_OS_MAP.get(args.os_label.lower(), args.os_label)
    canonical_platform = f"{canonical_os}-{canonical_arch}"

    # Defaults: full repo build (workflow_dispatch / scheduled triggers).
    build_dir = "build/all/Release"
    build_script_args = ""
    scope = "all"
    plugin_name = ""
    conan_hash_path = Path("conanfile.py")
    skip = False
    use_release_override = False
    needs_qt = False

    is_plugin_tag = args.tag_type == "tag" and "/" in args.tag

    if is_plugin_tag:
        plugin_name = args.tag.split("/", 1)[0]
        build_dir = f"build/{plugin_name}/Release"
        build_script_args = plugin_name
        scope = f"plugin-{plugin_name}"

        # Evaluated first so skipped entries don't trip on later checks (e.g. a missing conanfile.py
        # for plugins that override the build).
        manifest_path = Path(plugin_name) / "manifest.json"
        if manifest_path.is_file():
            manifest = json.loads(manifest_path.read_text())
            platforms = manifest.get("supported_platforms")
            needs_qt = manifest.get("category") == "widget"
            if platforms is not None and canonical_platform not in platforms:
                skip = True
                print(
                    f"::notice::Platform {canonical_platform} not in {manifest_path} "
                    f"supported_platforms — skipping this matrix entry",
                    file=sys.stderr,
                )

        # A release.sh override replaces the conan + cmake flow, so such plugins need no conanfile.py.
        release_script = Path(plugin_name) / "release.sh"
        if release_script.is_file() and release_script.stat().st_mode & 0o111:
            use_release_override = True
            print(
                f"::notice::Plugin {plugin_name} provides release.sh — using it instead of "
                f"the default build flow",
                file=sys.stderr,
            )

        # Cache-key source; skipped entries need no build inputs.
        if skip:
            conan_hash_path = manifest_path if manifest_path.is_file() else Path("conanfile.py")
        elif use_release_override:
            conan_hash_path = release_script
        else:
            conan_hash_path = Path(plugin_name) / "conanfile.py"
            if not conan_hash_path.is_file():
                print(
                    f"::error::Missing Conan recipe for tagged plugin: {conan_hash_path}",
                    file=sys.stderr,
                )
                return 1

    # Every recipe derives its plotjuggler_sdk pin from SDK_VERSION, so a core bump
    # must invalidate the cache even though the recipe's own bytes are unchanged.
    _conan_hash = hashlib.sha256()
    _conan_hash.update(conan_hash_path.read_bytes())
    _sdk_version_path = Path("SDK_VERSION")
    if _sdk_version_path.is_file():
        _conan_hash.update(_sdk_version_path.read_bytes())
    conan_hash = _conan_hash.hexdigest()

    outputs = {
        "build_dir": build_dir,
        "build_script_args": build_script_args,
        "conan_hash": conan_hash,
        "scope": scope,
        "plugin_name": plugin_name,
        "platform": canonical_platform,
        "skip": "true" if skip else "false",
        "use_release_override": "true" if use_release_override else "false",
        "needs_qt": "true" if needs_qt and not skip else "false",
    }
    for key, value in outputs.items():
        print(f"{key}={value}")

    print(
        f"Build scope: {scope} | dir={build_dir} | platform={canonical_platform} | "
        f"skip={skip} | release.sh override={use_release_override}",
        file=sys.stderr,
    )
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Release tools for PlotJuggler extensions",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    p_verify = subparsers.add_parser(
        "verify-version-consistency",
        help="Verify that tag, manifest.json, and plugin have matching versions",
        description="Compares versions from multiple sources to ensure release consistency. "
                    "Used in CI after building to catch version mismatches before publishing.",
    )
    p_verify.add_argument("source", nargs="?", help="Source directory name or extension id (manifest id)")
    p_verify.add_argument("--release-tag", help="Release tag (e.g., data_load_csv/v1.0.5). Extracts source and version automatically.")
    p_verify.add_argument("--build-dir", type=Path, help="Directory containing compiled plugins")
    p_verify.add_argument("--expected-version", help="Expected version (overrides tag version if both provided)")
    p_verify.add_argument("--check-manifest", action="store_true", help="Also validate manifest.json structure")
    p_verify.set_defaults(func=cmd_verify_version_consistency)

    p_package = subparsers.add_parser(
        "create-distribution-package",
        help="Create distribution package (extension) with plugin + manifest",
        description="Finds the compiled plugin by its embedded manifest id and copies it "
                    "along with manifest.json to the output directory. Outputs extension ZIP filename to stdout.",
    )
    p_package.add_argument("source", nargs="?", help="Source directory name or extension id (manifest id)")
    p_package.add_argument("--release-tag", help="Release tag (e.g., data_load_csv/v1.0.5). Extracts source and version automatically.")
    p_package.add_argument("--build-dir", type=Path, required=True, help="Directory containing compiled plugins")
    p_package.add_argument("--output-dir", type=Path, required=True, help="Output directory for extension")
    p_package.add_argument("--version", help="Version for ZIP filename (overrides tag version if both provided)")
    p_package.add_argument("--os-label", help="OS label for ZIP filename (linux, macos, windows)")
    p_package.add_argument("--arch", help="Architecture for ZIP filename (x86_64, arm64, etc.)")
    p_package.set_defaults(func=cmd_create_distribution_package)

    p_extract = subparsers.add_parser(
        "extract-embedded-manifest",
        help="Extract and display the manifest embedded in a compiled plugin",
        description="Loads a plugin binary via ctypes and extracts the JSON manifest "
                    "that was compiled into it. Useful for debugging version issues.",
    )
    p_extract.add_argument("plugin", help="Path to compiled plugin (.so/.dll/.dylib)")
    p_extract.set_defaults(func=cmd_extract_embedded_manifest)

    p_validate = subparsers.add_parser(
        "validate-manifest",
        help="Validate manifest.json structure and required fields",
        description="Checks that a manifest.json file has all required fields "
                    "and valid values (semver version, valid category, etc.).",
    )
    p_validate.add_argument("manifest", help="Path to manifest.json file")
    p_validate.set_defaults(func=cmd_validate_manifest)

    p_validate_pkg = subparsers.add_parser(
        "validate-distribution-package",
        help="Validate a distribution ZIP package (extension) completely",
        description="Performs comprehensive validation of an extension package: "
                    "filename format, SHA256 checksum, ZIP contents, manifest consistency "
                    "(file vs plugin), and filename vs content match.",
    )
    p_validate_pkg.add_argument("package", help="Path to extension ZIP file")
    p_validate_pkg.add_argument("--checksum-file", help="Path to .sha256 checksum file")
    p_validate_pkg.set_defaults(func=cmd_validate_distribution_package)

    p_notes = subparsers.add_parser(
        "generate-release-notes",
        help="Generate plugin-scoped release notes for a monorepo tag",
        description="Builds release notes from commits that touched the tagged plugin "
                    "directory. Shared build/runtime commits can be included explicitly.",
    )
    p_notes.add_argument("--release-tag", required=True, help="Release tag (e.g., data_load_csv/v1.0.5)")
    p_notes.add_argument("--previous-tag", help="Previous tag to diff against. Defaults to the previous tag for the same source directory.")
    p_notes.add_argument("--output", type=Path, help="Path where markdown release notes should be written")
    p_notes.add_argument("--include-shared", action="store_true", help="Also include shared build/runtime commits in a separate section")
    p_notes.set_defaults(func=cmd_generate_release_notes)

    p_scope = subparsers.add_parser(
        "resolve-build-scope",
        help="Resolve a CI matrix entry into GitHub Actions outputs",
        description="Computes build directory, conan recipe hash, plugin scope, "
                    "canonical platform, and the optional supported_platforms / "
                    "release.sh override flags. Outputs key=value lines on stdout; "
                    "the caller redirects them into $GITHUB_OUTPUT.",
    )
    p_scope.add_argument("--tag", default="", help="GITHUB_REF_NAME (empty when not on a tag)")
    p_scope.add_argument("--tag-type", default="", help="GITHUB_REF_TYPE (tag | branch | empty)")
    p_scope.add_argument("--os-label", required=True, help="matrix.os_label (linux | macos | windows)")
    p_scope.add_argument("--arch", required=True, help="matrix.arch (x86_64 | aarch64 | x64 | arm64)")
    p_scope.set_defaults(func=cmd_resolve_build_scope)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
