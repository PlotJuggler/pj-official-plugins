#!/usr/bin/env python3
"""Check that official plugins declare the SDK floor their source requires.

The scan roots are the targets passed to ``pj_emit_plugin_manifest`` in each
plugin CMakeLists.txt. Their ``target_link_libraries`` graph is followed through
plugin-local helper targets and targets declared under ``common/``. Once a
common target is reached, production C/C++ files in that common library's
directory are added to the plugin's scan closure.

This is deliberately a small CMake lexer rather than a CMake interpreter. It is
sound for this repository's layout: link targets are literal tokens, apart from
one helper function whose ``${TARGET}`` edge is conservatively attached to all
manifest-emitting targets in that plugin. Generator-expression/variable link
items are ignored. Tests, benchmarks, vendored ``contrib/`` code, and generated
build trees are not shipped plugin sources and are excluded from source scans.

``PJ_REQUIRE_SDK_VERSION(X, Y, Z)`` annotations are recognized in code or
comments and contribute to the same transitive closure maximum. The SDK-side
no-op macro does not exist yet; until it does, a comment annotation is the
compile-safe form.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from functools import total_ordering
from pathlib import Path
from typing import Iterable, Sequence, TextIO

from release_tools import SEMVER_REGEX


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_REPO_ROOT = SCRIPT_DIR.parent
MATCHING_RULE = "cpp_token_v1"

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inl",
    ".ipp",
    ".tpp",
}
EXCLUDED_SOURCE_DIRS = {
    ".git",
    ".pytest_cache",
    "benchmarks",
    "build",
    "contrib",
    "test",
    "test_data",
    "test_scripts",
    "tests",
}
LINK_KEYWORDS = {
    "PRIVATE",
    "PUBLIC",
    "INTERFACE",
    "LINK_PRIVATE",
    "LINK_PUBLIC",
    "debug",
    "general",
    "optimized",
}
ANNOTATION_NAME = "PJ_REQUIRE_SDK_VERSION"
ANNOTATION_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_])PJ_REQUIRE_SDK_VERSION(?![A-Za-z0-9_])")
ANNOTATION_RE = re.compile(
    r"(?<![A-Za-z0-9_])PJ_REQUIRE_SDK_VERSION\s*\(\s*"
    r"(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)"
)
COMMAND_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
LITERAL_TARGET_RE = re.compile(r"^[A-Za-z0-9_.:+-]+$")
RAW_STRING_START_RE = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')


class CheckError(Exception):
    """A configuration or repository shape error that must fail the check."""


@total_ordering
@dataclass(frozen=True)
class SemVer:
    """Comparable subset of SemVer accepted by release_tools.SEMVER_REGEX."""

    major: int
    minor: int
    patch: int
    prerelease: tuple[str, ...] = ()
    build: str | None = field(default=None, compare=False)
    original: str = field(default="", compare=False)

    @classmethod
    def parse(cls, value: str, context: str) -> "SemVer":
        if not isinstance(value, str):
            raise CheckError(f"{context} must be a semantic-version string")
        match = SEMVER_REGEX.fullmatch(value)
        if not match:
            raise CheckError(
                f"{context} has invalid version {value!r}; expected X.Y.Z with optional pre-release"
            )
        prerelease = tuple(match.group(4).split(".")) if match.group(4) else ()
        return cls(
            int(match.group(1)),
            int(match.group(2)),
            int(match.group(3)),
            prerelease,
            match.group(5),
            value,
        )

    @classmethod
    def from_annotation(cls, major: str, minor: str, patch: str) -> "SemVer":
        value = f"{int(major)}.{int(minor)}.{int(patch)}"
        return cls(int(major), int(minor), int(patch), original=value)

    def __str__(self) -> str:
        if self.original:
            return self.original
        result = f"{self.major}.{self.minor}.{self.patch}"
        if self.prerelease:
            result += "-" + ".".join(self.prerelease)
        if self.build:
            result += "+" + self.build
        return result

    def __lt__(self, other: object) -> bool:
        if not isinstance(other, SemVer):
            return NotImplemented
        core_self = (self.major, self.minor, self.patch)
        core_other = (other.major, other.minor, other.patch)
        if core_self != core_other:
            return core_self < core_other
        return _compare_prerelease(self.prerelease, other.prerelease) < 0


def _compare_prerelease(left: tuple[str, ...], right: tuple[str, ...]) -> int:
    if not left and not right:
        return 0
    if not left:
        return 1
    if not right:
        return -1

    for left_part, right_part in zip(left, right):
        if left_part == right_part:
            continue
        left_numeric = left_part.isdigit()
        right_numeric = right_part.isdigit()
        if left_numeric and right_numeric:
            return -1 if int(left_part) < int(right_part) else 1
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return -1 if left_part < right_part else 1

    if len(left) == len(right):
        return 0
    return -1 if len(left) < len(right) else 1


@dataclass(frozen=True)
class FeatureFloor:
    identifier: str
    minimum: SemVer
    matcher: re.Pattern[str]


@dataclass(frozen=True)
class Trigger:
    minimum: SemVer
    kind: str
    identifier: str
    path: Path
    line: int


@dataclass
class CMakeModel:
    targets: set[str] = field(default_factory=set)
    release_targets: set[str] = field(default_factory=set)
    edges: dict[str, set[str]] = field(default_factory=lambda: defaultdict(set))
    dynamic_link_dependencies: set[str] = field(default_factory=set)


@dataclass
class CommonGraph:
    target_dirs: dict[str, Path]
    edges: dict[str, set[str]]


def _reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise CheckError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _feature_matcher(identifier: str) -> re.Pattern[str]:
    call = identifier.endswith("(")
    literal = identifier[:-1] if call else identifier
    if not literal:
        raise CheckError("feature identifiers must not be empty")

    pattern = re.escape(literal)
    if literal[0].isalnum() or literal[0] == "_":
        pattern = r"(?<![A-Za-z0-9_])" + pattern
    if call:
        pattern += r"\s*\("
    elif literal[-1].isalnum() or literal[-1] == "_":
        pattern += r"(?![A-Za-z0-9_])"
    return re.compile(pattern)


def load_feature_floors(path: Path) -> list[FeatureFloor]:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CheckError(f"cannot read feature-floor map {path}: {exc}") from exc

    try:
        document = json.loads(raw, object_pairs_hook=_reject_duplicate_json_keys)
    except json.JSONDecodeError as exc:
        raise CheckError(f"invalid JSON in feature-floor map {path}: {exc}") from exc

    if not isinstance(document, dict):
        raise CheckError(f"feature-floor map {path} must contain a JSON object")
    expected_keys = {"schema_version", "matching_rule", "_comment", "features"}
    unknown_keys = set(document) - expected_keys
    if unknown_keys:
        raise CheckError(
            f"feature-floor map {path} has unknown top-level keys: {', '.join(sorted(unknown_keys))}"
        )
    if document.get("schema_version") != 1:
        raise CheckError(f"feature-floor map {path} must use schema_version 1")
    if document.get("matching_rule") != MATCHING_RULE:
        raise CheckError(
            f"feature-floor map {path} must use matching_rule {MATCHING_RULE!r}"
        )
    if "_comment" in document and not isinstance(document["_comment"], str):
        raise CheckError(f"feature-floor map {path} _comment must be a string")

    features = document.get("features")
    if not isinstance(features, dict) or not features:
        raise CheckError(f"feature-floor map {path} features must be a non-empty object")

    floors = []
    for identifier, minimum_text in features.items():
        if not isinstance(identifier, str) or not identifier.strip():
            raise CheckError(f"feature-floor map {path} contains an empty identifier")
        if identifier != identifier.strip() or "\n" in identifier or "\r" in identifier:
            raise CheckError(f"feature identifier {identifier!r} must be a single trimmed token")
        minimum = SemVer.parse(
            minimum_text,
            f"minimum SDK version for feature {identifier!r} in {path}",
        )
        floors.append(FeatureFloor(identifier, minimum, _feature_matcher(identifier)))
    return floors


def _blank_except_newlines(text: str) -> str:
    return "".join("\n" if char == "\n" else " " for char in text)


def _strip_cmake_comments(text: str) -> str:
    output: list[str] = []
    index = 0
    quoted = False
    escaped = False
    while index < len(text):
        char = text[index]
        if quoted:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            index += 1
            continue

        if char == '"':
            quoted = True
            output.append(char)
            index += 1
            continue
        if char == "#":
            bracket = re.match(r"#\[(=*)\[", text[index:])
            if bracket:
                closing = "]" + bracket.group(1) + "]"
                end = text.find(closing, index + bracket.end())
                end = len(text) if end < 0 else end + len(closing)
            else:
                end = text.find("\n", index)
                end = len(text) if end < 0 else end
            output.append(_blank_except_newlines(text[index:end]))
            index = end
            continue
        output.append(char)
        index += 1
    return "".join(output)


def _cmake_commands(path: Path) -> list[tuple[str, str]]:
    try:
        text = _strip_cmake_comments(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise CheckError(f"cannot read {path}: {exc}") from exc

    commands: list[tuple[str, str]] = []
    index = 0
    while True:
        match = COMMAND_NAME_RE.search(text, index)
        if not match:
            break
        cursor = match.end()
        while cursor < len(text) and text[cursor].isspace():
            cursor += 1
        if cursor >= len(text) or text[cursor] != "(":
            index = match.end()
            continue

        body_start = cursor + 1
        cursor = body_start
        depth = 1
        quoted = False
        escaped = False
        while cursor < len(text) and depth:
            char = text[cursor]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            cursor += 1
        if depth:
            raise CheckError(f"unbalanced CMake command {match.group(0)} in {path}")
        commands.append((match.group(0).lower(), text[body_start : cursor - 1]))
        index = cursor
    return commands


def _cmake_tokens(body: str, path: Path) -> list[str]:
    lexer = shlex.shlex(body, posix=True)
    lexer.whitespace_split = True
    lexer.commenters = ""
    try:
        raw_tokens = list(lexer)
    except ValueError as exc:
        raise CheckError(f"cannot tokenize a CMake command in {path}: {exc}") from exc
    return [item for token in raw_tokens for item in token.split(";") if item]


def _link_dependencies(tokens: Sequence[str]) -> set[str]:
    dependencies = set()
    for token in tokens:
        if token in LINK_KEYWORDS or token.startswith("-"):
            continue
        if token.startswith("${") or token.startswith("$<"):
            continue
        if LITERAL_TARGET_RE.fullmatch(token):
            dependencies.add(token)
    return dependencies


def parse_cmake_model(path: Path) -> CMakeModel:
    model = CMakeModel()
    for name, body in _cmake_commands(path):
        tokens = _cmake_tokens(body, path)
        if not tokens:
            continue
        if name == "add_library" and LITERAL_TARGET_RE.fullmatch(tokens[0]):
            model.targets.add(tokens[0])
        elif name == "pj_emit_plugin_manifest" and LITERAL_TARGET_RE.fullmatch(tokens[0]):
            model.release_targets.add(tokens[0])
        elif name == "target_link_libraries" and len(tokens) > 1:
            dependencies = _link_dependencies(tokens[1:])
            source = tokens[0]
            if LITERAL_TARGET_RE.fullmatch(source):
                model.edges[source].update(dependencies)
            else:
                model.dynamic_link_dependencies.update(dependencies)
    return model


def load_common_graph(repo_root: Path) -> CommonGraph:
    common_root = repo_root / "common"
    if not common_root.is_dir():
        return CommonGraph({}, {})

    models: list[tuple[Path, CMakeModel]] = []
    target_dirs: dict[str, Path] = {}
    for cmake_path in sorted(common_root.glob("*/CMakeLists.txt")):
        model = parse_cmake_model(cmake_path)
        models.append((cmake_path.parent, model))
        for target in model.targets:
            previous = target_dirs.get(target)
            if previous is not None and previous != cmake_path.parent:
                raise CheckError(
                    f"common CMake target {target!r} is declared in both {previous} and {cmake_path.parent}"
                )
            target_dirs[target] = cmake_path.parent

    edges: dict[str, set[str]] = defaultdict(set)
    for _, model in models:
        for source, dependencies in model.edges.items():
            edges[source].update(dependencies)
        if model.dynamic_link_dependencies:
            for target in model.targets:
                edges[target].update(model.dynamic_link_dependencies)
    return CommonGraph(target_dirs, edges)


def derive_common_closure(plugin_dir: Path, common_graph: CommonGraph) -> set[Path]:
    cmake_path = plugin_dir / "CMakeLists.txt"
    if not cmake_path.is_file():
        raise CheckError(f"plugin {plugin_dir} has a manifest.json but no CMakeLists.txt")
    plugin_model = parse_cmake_model(cmake_path)
    if not plugin_model.release_targets:
        raise CheckError(
            f"plugin {plugin_dir} has no pj_emit_plugin_manifest(...) release target"
        )

    edges: dict[str, set[str]] = defaultdict(set)
    for source, dependencies in common_graph.edges.items():
        edges[source].update(dependencies)
    for source, dependencies in plugin_model.edges.items():
        edges[source].update(dependencies)
    if plugin_model.dynamic_link_dependencies:
        for target in plugin_model.release_targets:
            edges[target].update(plugin_model.dynamic_link_dependencies)

    common_dirs: set[Path] = set()
    pending = list(plugin_model.release_targets)
    visited: set[str] = set()
    while pending:
        target = pending.pop()
        if target in visited:
            continue
        visited.add(target)
        common_dir = common_graph.target_dirs.get(target)
        if common_dir is not None:
            common_dirs.add(common_dir)
        pending.extend(edges.get(target, ()))
    return common_dirs


def _strip_cpp_comments(text: str) -> str:
    """Remove C/C++ comments while preserving source offsets and string literals."""
    output: list[str] = []
    index = 0
    quote: str | None = None
    escaped = False
    while index < len(text):
        if quote is not None:
            char = text[index]
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            index += 1
            continue

        raw_match = RAW_STRING_START_RE.match(text, index)
        if raw_match:
            closing = ")" + raw_match.group(1) + '"'
            end = text.find(closing, raw_match.end())
            end = len(text) if end < 0 else end + len(closing)
            output.append(text[index:end])
            index = end
            continue
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
            output.append(_blank_except_newlines(text[index:end]))
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
            output.append(_blank_except_newlines(text[index:end]))
            index = end
            continue
        char = text[index]
        if char in {'"', "'"}:
            quote = char
        output.append(char)
        index += 1
    return "".join(output)


def _source_files(directory: Path) -> Iterable[Path]:
    for path in directory.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        relative_parts = path.relative_to(directory).parts[:-1]
        if any(part in EXCLUDED_SOURCE_DIRS or part.startswith("build-") for part in relative_parts):
            continue
        yield path


def _display_path(path: Path, repo_root: Path) -> str:
    try:
        return path.relative_to(repo_root).as_posix()
    except ValueError:
        return str(path)


def detect_triggers(
    repo_root: Path,
    scan_dirs: Iterable[Path],
    feature_floors: Sequence[FeatureFloor],
) -> list[Trigger]:
    triggers: set[Trigger] = set()
    source_files = sorted({path for directory in scan_dirs for path in _source_files(directory)})
    for path in source_files:
        try:
            raw_source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            raise CheckError(f"cannot read C/C++ source {_display_path(path, repo_root)}: {exc}") from exc
        searchable_source = _strip_cpp_comments(raw_source)
        for feature in feature_floors:
            for match in feature.matcher.finditer(searchable_source):
                triggers.add(
                    Trigger(
                        feature.minimum,
                        "identifier",
                        feature.identifier,
                        path,
                        raw_source.count("\n", 0, match.start()) + 1,
                    )
                )

        annotation_matches = list(ANNOTATION_RE.finditer(raw_source))
        valid_starts = {match.start() for match in annotation_matches}
        for token_match in ANNOTATION_TOKEN_RE.finditer(raw_source):
            if token_match.start() not in valid_starts:
                line = raw_source.count("\n", 0, token_match.start()) + 1
                raise CheckError(
                    f"malformed {ANNOTATION_NAME}(X, Y, Z) annotation in "
                    f"{_display_path(path, repo_root)}:{line}"
                )
        for match in annotation_matches:
            minimum = SemVer.from_annotation(*match.groups())
            triggers.add(
                Trigger(
                    minimum,
                    "annotation",
                    match.group(0),
                    path,
                    raw_source.count("\n", 0, match.start()) + 1,
                )
            )

    return sorted(
        triggers,
        key=lambda item: (
            item.minimum,
            _display_path(item.path, repo_root),
            item.line,
            item.kind,
            item.identifier,
        ),
    )


def _read_pinned_sdk_version(repo_root: Path) -> SemVer:
    path = repo_root / "SDK_VERSION"
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise CheckError(f"cannot read pinned SDK version {path}: {exc}") from exc
    return SemVer.parse(value, f"pinned SDK_VERSION in {path}")


def _read_manifest_floor(plugin_dir: Path) -> SemVer | None:
    path = plugin_dir / "manifest.json"
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise CheckError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise CheckError(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(manifest, dict):
        raise CheckError(f"{path} must contain a JSON object")
    if "min_sdk_required" not in manifest:
        return None
    value = manifest["min_sdk_required"]
    return SemVer.parse(value, f"min_sdk_required in {path}")


def discover_plugin_dirs(repo_root: Path) -> list[Path]:
    return sorted(path.parent for path in repo_root.glob("*/manifest.json"))


def _resolve_plugin_dirs(repo_root: Path, plugins: Sequence[Path] | None) -> list[Path]:
    if plugins is None:
        result = discover_plugin_dirs(repo_root)
    else:
        result = []
        for plugin in plugins:
            candidate = plugin if plugin.is_absolute() else repo_root / plugin
            if candidate.name == "manifest.json":
                candidate = candidate.parent
            result.append(candidate.resolve())
    unique = sorted(set(result))
    if not unique:
        raise CheckError(f"no plugin directories with manifest.json found under {repo_root}")
    for plugin_dir in unique:
        if not (plugin_dir / "manifest.json").is_file():
            raise CheckError(f"plugin directory {plugin_dir} does not contain manifest.json")
    return unique


def check_sdk_feature_floors(
    repo_root: Path = DEFAULT_REPO_ROOT,
    plugins: Sequence[Path] | None = None,
    floors_path: Path | None = None,
    *,
    out: TextIO = sys.stdout,
    err: TextIO = sys.stderr,
) -> bool:
    """Run the guardrail and return True only when every selected plugin passes."""
    repo_root = repo_root.resolve()
    floors_path = floors_path or repo_root / "scripts" / "sdk_feature_floors.json"
    if not floors_path.is_absolute():
        floors_path = repo_root / floors_path

    try:
        feature_floors = load_feature_floors(floors_path)
        sdk_version = _read_pinned_sdk_version(repo_root)
        plugin_dirs = _resolve_plugin_dirs(repo_root, plugins)
        common_graph = load_common_graph(repo_root)
    except CheckError as exc:
        print(f"ERROR: {exc}", file=err)
        return False

    failed = 0
    for plugin_dir in plugin_dirs:
        plugin_name = _display_path(plugin_dir, repo_root)
        try:
            manifest_floor = _read_manifest_floor(plugin_dir)
            common_dirs = derive_common_closure(plugin_dir, common_graph)
            triggers = detect_triggers(
                repo_root,
                [plugin_dir, *sorted(common_dirs)],
                feature_floors,
            )
        except CheckError as exc:
            failed += 1
            print(f"FAIL {plugin_name}: {exc}", file=err)
            continue

        problems: list[str] = []
        if manifest_floor is not None and manifest_floor > sdk_version:
            problems.append(
                f"manifest min_sdk_required {manifest_floor} is newer than pinned SDK_VERSION {sdk_version}"
            )

        effective_floor = max((trigger.minimum for trigger in triggers), default=None)
        underdeclared = effective_floor is not None and (
            manifest_floor is None or manifest_floor < effective_floor
        )
        if underdeclared:
            declared = "absent" if manifest_floor is None else str(manifest_floor)
            problems.append(
                f"effective SDK floor {effective_floor} exceeds manifest min_sdk_required {declared}"
            )

        if problems:
            failed += 1
            print(f"FAIL {plugin_name}:", file=err)
            for problem in problems:
                print(f"  - {problem}", file=err)
            if underdeclared:
                print("  Detected floor inputs:", file=err)
                for trigger in triggers:
                    location = f"{_display_path(trigger.path, repo_root)}:{trigger.line}"
                    print(
                        f"    - {trigger.minimum} {trigger.kind} {trigger.identifier!r} at {location}",
                        file=err,
                    )
            continue

        if effective_floor is None:
            print(f"PASS {plugin_name}: no SDK feature usage detected", file=out)
        else:
            print(
                f"PASS {plugin_name}: effective SDK floor {effective_floor} "
                f"<= min_sdk_required {manifest_floor}",
                file=out,
            )

    if failed:
        print(
            f"SDK feature-floor check failed: {failed} of {len(plugin_dirs)} plugins failed "
            f"(pinned SDK {sdk_version}).",
            file=err,
        )
        return False
    print(
        f"SDK feature-floor check passed: {len(plugin_dirs)} plugins (pinned SDK {sdk_version}).",
        file=out,
    )
    return True


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate plugin min_sdk_required declarations against detected SDK features."
    )
    parser.add_argument(
        "plugins",
        nargs="*",
        type=Path,
        help="Plugin directories to check relative to --root (default: every top-level manifest.json)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=DEFAULT_REPO_ROOT,
        help="Repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--floors-file",
        type=Path,
        help="Feature-floor JSON path, absolute or relative to --root",
    )
    args = parser.parse_args(argv)
    selected_plugins = args.plugins or None
    return 0 if check_sdk_feature_floors(args.root, selected_plugins, args.floors_file) else 1


if __name__ == "__main__":
    raise SystemExit(main())
