#!/usr/bin/env python3
"""Enforce each plugin's ``min_sdk_required`` against the host surfaces it uses.

The floor a plugin declares is a HOST-CONTRACT claim: only surfaces the host
provides at runtime (vtable tail slots, named services, wire contracts) can
constrain it. Static-library SDK helpers compile into the plugin and never do.
The authoritative surface -> introducing-version table therefore lives in the
SDK (``pj_base/feature_floors.json``, shipped with the package from 0.33.0);
until the pinned ``SDK_VERSION`` carries it, a bundled interim copy stands in
(``scripts/sdk_feature_floors_interim.json``) and the check FAILS if both
exist and differ.

Per plugin, over its source closure (the plugin directory plus every
``common/`` library reachable from its ``pj_emit_plugin_manifest`` targets
through ``target_link_libraries`` — unresolved link expressions fail closed):

    declared min_sdk_required >= max(since) of every matched surface

unless the manifest carries a validated ``sdk_floor_exceptions`` entry for
that surface:

    "sdk_floor_exceptions": {
      "setDatasetMetadata": {
        "reason": "optional: metadata display absent on hosts older than 0.32",
        "test": "SuiteName.TestName"
      }
    }

An exception is valid only when the identifier is in the table, the surface is
runtime-negotiated (a hard protocol surface can never be waived), the
identifier actually matches in this plugin's closure (a stale claim fails),
and the named gtest exists in the plugin's sources. Floors below the table's
``minimum_supported_floor`` fail; a floor higher than anything matched only
warns (the maintainer may know a runtime reason the scan cannot see).

The scan is comment-stripped token matching with identifier boundaries — the
whole surface universe is a few dozen identifiers, so token precision is
sufficient; a Clang-based extractor is the documented upgrade path if a real
ambiguity ever appears.
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
INTERIM_TABLE_NAME = "sdk_feature_floors_interim.json"
SDK_TABLE_RELATIVE = Path("pj_base") / "feature_floors.json"

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
    "demo",
    "test",
    "test_data",
    "test_scripts",
    "tests",
}
TEST_SOURCE_DIRS = {"test", "tests"}
# These CMake variables are platform-selected external libraries, never targets
# implemented under common/. Keeping the exceptions explicit makes a newly
# introduced unresolved expression fail closed instead of silently shrinking a
# plugin's source closure.
EXTERNAL_LINK_EXPRESSION_ALLOWLIST = {
    "${CMAKE_DL_LIBS}": "CMake's platform dynamic-loader libraries",
    "${MOSAICO_GRPCPP_TARGET}": "optional external gRPC target selected by toolbox_mosaico",
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
COMMAND_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
LITERAL_TARGET_RE = re.compile(r"^[A-Za-z0-9_.:+-]+$")
VARIABLE_REF_RE = re.compile(r"^\$\{([A-Za-z_][A-Za-z0-9_]*)\}$")
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
class Surface:
    identifier: str
    since: SemVer
    negotiated: bool
    declaration: str
    matcher: re.Pattern[str]


@dataclass(frozen=True)
class SurfaceTable:
    surfaces: dict[str, Surface]
    minimum_supported_floor: SemVer
    path: Path


@dataclass(frozen=True)
class Trigger:
    surface: Surface
    path: Path
    line: int


@dataclass(frozen=True)
class UnresolvedLink:
    expression: str
    cmake_path: Path


@dataclass
class CMakeModel:
    targets: set[str] = field(default_factory=set)
    release_targets: set[str] = field(default_factory=set)
    edges: dict[str, set[str]] = field(default_factory=lambda: defaultdict(set))
    unresolved_edges: dict[str, set[UnresolvedLink]] = field(
        default_factory=lambda: defaultdict(set)
    )
    dynamic_link_dependencies: set[str] = field(default_factory=set)
    dynamic_unresolved_links: set[UnresolvedLink] = field(default_factory=set)


@dataclass
class CommonGraph:
    target_dirs: dict[str, Path]
    edges: dict[str, set[str]]
    unresolved_edges: dict[str, set[UnresolvedLink]]


def _reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise CheckError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _surface_matcher(identifier: str) -> re.Pattern[str]:
    if not identifier:
        raise CheckError("surface identifiers must not be empty")
    pattern = re.escape(identifier)
    if identifier[0].isalnum() or identifier[0] == "_":
        pattern = r"(?<![A-Za-z0-9_])" + pattern
    if identifier[-1].isalnum() or identifier[-1] == "_":
        pattern += r"(?![A-Za-z0-9_])"
    return re.compile(pattern)


def load_surface_table(path: Path) -> SurfaceTable:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CheckError(f"cannot read surface table {path}: {exc}") from exc

    try:
        document = json.loads(raw, object_pairs_hook=_reject_duplicate_json_keys)
    except json.JSONDecodeError as exc:
        raise CheckError(f"invalid JSON in surface table {path}: {exc}") from exc

    if not isinstance(document, dict):
        raise CheckError(f"surface table {path} must contain a JSON object")
    expected_keys = {"schema_version", "minimum_supported_floor", "_comment", "doc", "surfaces", "baseline"}
    unknown_keys = set(document) - expected_keys
    if unknown_keys:
        raise CheckError(
            f"surface table {path} has unknown top-level keys: {', '.join(sorted(unknown_keys))}"
        )
    if document.get("schema_version") != 1:
        raise CheckError(f"surface table {path} must use schema_version 1")

    minimum_supported_floor = SemVer.parse(
        document.get("minimum_supported_floor"),
        f"minimum_supported_floor in {path}",
    )

    surfaces_raw = document.get("surfaces")
    if not isinstance(surfaces_raw, dict) or not surfaces_raw:
        raise CheckError(f"surface table {path} surfaces must be a non-empty object")

    surfaces: dict[str, Surface] = {}
    for identifier, entry in surfaces_raw.items():
        if not isinstance(identifier, str) or identifier != identifier.strip() or not identifier:
            raise CheckError(f"surface identifier {identifier!r} must be a single trimmed token")
        if not isinstance(entry, dict):
            raise CheckError(f"surface {identifier!r} in {path} must be an object")
        unknown = set(entry) - {"since", "negotiated", "declaration"}
        if unknown:
            raise CheckError(
                f"surface {identifier!r} in {path} has unknown keys: {', '.join(sorted(unknown))}"
            )
        since = SemVer.parse(entry.get("since"), f"since for surface {identifier!r} in {path}")
        negotiated = entry.get("negotiated")
        if not isinstance(negotiated, bool):
            raise CheckError(f"surface {identifier!r} in {path} needs a boolean 'negotiated'")
        declaration = entry.get("declaration")
        if not isinstance(declaration, str) or not declaration:
            raise CheckError(f"surface {identifier!r} in {path} needs a non-empty 'declaration'")
        surfaces[identifier] = Surface(
            identifier, since, negotiated, declaration, _surface_matcher(identifier)
        )

    # Baseline surfaces: present at or before minimum_supported_floor, so they
    # can never raise a floor at or above it — listed so verdicts can name them
    # and so an exception naming one is "known" (and flagged as unnecessary).
    baseline_raw = document.get("baseline", [])
    if not isinstance(baseline_raw, list):
        raise CheckError(f"surface table {path} baseline must be an array of identifiers")
    for identifier in baseline_raw:
        if not isinstance(identifier, str) or identifier != identifier.strip() or not identifier:
            raise CheckError(f"baseline identifier {identifier!r} must be a single trimmed token")
        if identifier in surfaces:
            raise CheckError(
                f"surface {identifier!r} in {path} is listed in both surfaces and baseline"
            )
        surfaces[identifier] = Surface(
            identifier, minimum_supported_floor, True, identifier, _surface_matcher(identifier)
        )
    return SurfaceTable(surfaces, minimum_supported_floor, path)


def _surfaces_payload(path: Path) -> object:
    document = json.loads(path.read_text(encoding="utf-8"))
    return {
        "minimum_supported_floor": document.get("minimum_supported_floor"),
        "surfaces": document.get("surfaces"),
        "baseline": document.get("baseline", []),
    }


def resolve_surface_table(
    repo_root: Path,
    sdk_floors_path: Path | None,
    *,
    err: TextIO,
) -> SurfaceTable:
    """Prefer the SDK's own table; fall back to the bundled interim copy.

    When both exist they must agree byte-for-payload — a diverging interim
    copy is a rot hazard and fails the check outright (sync or delete it).
    """
    interim_path = SCRIPT_DIR / INTERIM_TABLE_NAME
    if not repo_root.samefile(DEFAULT_REPO_ROOT):
        candidate = repo_root / "scripts" / INTERIM_TABLE_NAME
        interim_path = candidate if candidate.is_file() else interim_path

    if sdk_floors_path is not None:
        if not sdk_floors_path.is_file():
            raise CheckError(f"--sdk-floors table not found: {sdk_floors_path}")
        table = load_surface_table(sdk_floors_path)
        if interim_path.is_file():
            if _surfaces_payload(sdk_floors_path) != _surfaces_payload(interim_path):
                raise CheckError(
                    f"interim table {interim_path} differs from the SDK table "
                    f"{sdk_floors_path}; sync it or delete it (the SDK copy wins)"
                )
            print(
                f"NOTE: SDK surface table in use; delete the interim copy {interim_path}",
                file=err,
            )
        return table

    if not interim_path.is_file():
        raise CheckError(
            f"no surface table: pass --sdk-floors <path to {SDK_TABLE_RELATIVE}> "
            f"or restore scripts/{INTERIM_TABLE_NAME}"
        )
    print(
        f"WARNING: using interim surface table {interim_path.name} "
        f"(authoritative copy ships with plotjuggler_sdk >= 0.33.0)",
        file=err,
    )
    return load_surface_table(interim_path)


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


def _resolve_link_item(
    token: str,
    variables: dict[str, set[str]],
    cmake_path: Path,
    resolving: tuple[str, ...] = (),
) -> tuple[set[str], set[UnresolvedLink]]:
    if token in LINK_KEYWORDS or token.startswith("-"):
        return set(), set()
    if token in EXTERNAL_LINK_EXPRESSION_ALLOWLIST:
        return set(), set()
    if LITERAL_TARGET_RE.fullmatch(token):
        return {token}, set()

    variable_match = VARIABLE_REF_RE.fullmatch(token)
    if not variable_match:
        return set(), {UnresolvedLink(token, cmake_path)}

    variable_name = variable_match.group(1)
    if variable_name in resolving:
        return set(), {UnresolvedLink(token, cmake_path)}
    if variable_name not in variables:
        return set(), {UnresolvedLink(token, cmake_path)}

    dependencies: set[str] = set()
    unresolved: set[UnresolvedLink] = set()
    for value in variables[variable_name]:
        value_dependencies, value_unresolved = _resolve_link_item(
            value,
            variables,
            cmake_path,
            (*resolving, variable_name),
        )
        dependencies.update(value_dependencies)
        unresolved.update(value_unresolved)
    return dependencies, unresolved


def parse_cmake_model(path: Path) -> CMakeModel:
    model = CMakeModel()
    commands = [(name, _cmake_tokens(body, path)) for name, body in _cmake_commands(path)]
    variables: dict[str, set[str]] = {}
    for name, tokens in commands:
        if name != "set" or not tokens or not COMMAND_NAME_RE.fullmatch(tokens[0]):
            continue
        variables.setdefault(tokens[0], set()).update(tokens[1:])

    for name, tokens in commands:
        if not tokens:
            continue
        if name == "add_library" and LITERAL_TARGET_RE.fullmatch(tokens[0]):
            model.targets.add(tokens[0])
            if len(tokens) >= 3 and tokens[1].upper() == "ALIAS":
                alias_target = tokens[2]
                if LITERAL_TARGET_RE.fullmatch(alias_target):
                    model.edges[tokens[0]].add(alias_target)
                else:
                    model.unresolved_edges[tokens[0]].add(UnresolvedLink(alias_target, path))
        elif name == "pj_emit_plugin_manifest" and LITERAL_TARGET_RE.fullmatch(tokens[0]):
            model.release_targets.add(tokens[0])
        elif name == "target_link_libraries" and len(tokens) > 1:
            dependencies: set[str] = set()
            unresolved: set[UnresolvedLink] = set()
            for token in tokens[1:]:
                token_dependencies, token_unresolved = _resolve_link_item(token, variables, path)
                dependencies.update(token_dependencies)
                unresolved.update(token_unresolved)
            source = tokens[0]
            if LITERAL_TARGET_RE.fullmatch(source):
                model.edges[source].update(dependencies)
                model.unresolved_edges[source].update(unresolved)
            else:
                source_targets, _ = _resolve_link_item(source, variables, path)
                if source_targets:
                    for source_target in source_targets:
                        model.edges[source_target].update(dependencies)
                        model.unresolved_edges[source_target].update(unresolved)
                else:
                    # Function parameters such as parser_ros's ${TARGET} are
                    # attached conservatively to every release target below.
                    model.dynamic_link_dependencies.update(dependencies)
                    model.dynamic_unresolved_links.update(unresolved)
    return model


def load_common_graph(repo_root: Path) -> CommonGraph:
    common_root = repo_root / "common"
    if not common_root.is_dir():
        return CommonGraph({}, {}, {})

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
    unresolved_edges: dict[str, set[UnresolvedLink]] = defaultdict(set)
    for _, model in models:
        for source, dependencies in model.edges.items():
            edges[source].update(dependencies)
        for source, unresolved in model.unresolved_edges.items():
            unresolved_edges[source].update(unresolved)
        if model.dynamic_link_dependencies:
            for target in model.targets:
                edges[target].update(model.dynamic_link_dependencies)
        if model.dynamic_unresolved_links:
            for target in model.targets:
                unresolved_edges[target].update(model.dynamic_unresolved_links)
    return CommonGraph(target_dirs, edges, unresolved_edges)


def derive_common_closure(plugin_dir: Path, common_graph: CommonGraph) -> set[Path]:
    cmake_path = plugin_dir / "CMakeLists.txt"
    if not cmake_path.is_file():
        raise CheckError(f"plugin {plugin_dir} has a manifest.json but no CMakeLists.txt")
    plugin_model = parse_cmake_model(cmake_path)
    if not plugin_model.release_targets:
        raise CheckError(f"plugin {plugin_dir} has no pj_emit_plugin_manifest(...) release target")

    edges: dict[str, set[str]] = defaultdict(set)
    unresolved_edges: dict[str, set[UnresolvedLink]] = defaultdict(set)
    for source, dependencies in common_graph.edges.items():
        edges[source].update(dependencies)
    for source, unresolved in common_graph.unresolved_edges.items():
        unresolved_edges[source].update(unresolved)
    for source, dependencies in plugin_model.edges.items():
        edges[source].update(dependencies)
    for source, unresolved in plugin_model.unresolved_edges.items():
        unresolved_edges[source].update(unresolved)
    if plugin_model.dynamic_link_dependencies:
        for target in plugin_model.release_targets:
            edges[target].update(plugin_model.dynamic_link_dependencies)
    if plugin_model.dynamic_unresolved_links:
        for target in plugin_model.release_targets:
            unresolved_edges[target].update(plugin_model.dynamic_unresolved_links)

    common_dirs: set[Path] = set()
    pending = list(plugin_model.release_targets)
    visited: set[str] = set()
    while pending:
        target = pending.pop()
        if target in visited:
            continue
        visited.add(target)
        unresolved = unresolved_edges.get(target, set())
        if unresolved:
            details = ", ".join(
                f"{item.expression!r} in {item.cmake_path}"
                for item in sorted(
                    unresolved,
                    key=lambda item: (str(item.cmake_path), item.expression),
                )
            )
            raise CheckError(
                f"reachable CMake target {target!r} has unresolved link items: {details}; "
                "resolve them with set(), an ALIAS target, or the explicit external allowlist"
            )
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


def _splice_cpp_lines(text: str) -> tuple[str, list[int]]:
    """Apply C/C++ phase-2 backslash-newline removal and retain offset mapping."""
    output: list[str] = []
    original_offsets: list[int] = []
    index = 0
    while index < len(text):
        if text.startswith("\\\r\n", index):
            index += 3
            continue
        if text.startswith("\\\n", index):
            index += 2
            continue
        output.append(text[index])
        original_offsets.append(index)
        index += 1
    return "".join(output), original_offsets


def _original_line(text: str, original_offsets: Sequence[int], spliced_offset: int) -> int:
    original_offset = (
        original_offsets[spliced_offset] if spliced_offset < len(original_offsets) else len(text)
    )
    return text.count("\n", 0, original_offset) + 1


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
    table: SurfaceTable,
) -> list[Trigger]:
    triggers: set[Trigger] = set()
    source_files = sorted({path for directory in scan_dirs for path in _source_files(directory)})
    for path in source_files:
        try:
            raw_source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            raise CheckError(
                f"cannot read C/C++ source {_display_path(path, repo_root)}: {exc}"
            ) from exc
        spliced_source, original_offsets = _splice_cpp_lines(raw_source)
        searchable_source = _strip_cpp_comments(spliced_source)
        for surface in table.surfaces.values():
            for match in surface.matcher.finditer(searchable_source):
                triggers.add(
                    Trigger(
                        surface,
                        path,
                        _original_line(raw_source, original_offsets, match.start()),
                    )
                )
    return sorted(
        triggers,
        key=lambda item: (
            item.surface.since,
            item.surface.identifier,
            _display_path(item.path, repo_root),
            item.line,
        ),
    )


def _test_exists(plugin_dir: Path, test_name: str) -> bool:
    """True when `Suite.Name` resolves to a gtest TEST/TEST_F/TEST_P in the plugin."""
    if "." not in test_name:
        return False
    suite, _, name = test_name.partition(".")
    pattern = re.compile(
        r"TEST(?:_F|_P)?\s*\(\s*" + re.escape(suite) + r"\s*,\s*" + re.escape(name) + r"\s*[,)]"
    )
    for directory in sorted(plugin_dir.iterdir()):
        if not directory.is_dir() or directory.name not in TEST_SOURCE_DIRS:
            continue
        for path in directory.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            try:
                if pattern.search(path.read_text(encoding="utf-8")):
                    return True
            except (OSError, UnicodeDecodeError):
                continue
    return False


def _read_manifest(plugin_dir: Path) -> dict:
    path = plugin_dir / "manifest.json"
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise CheckError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise CheckError(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(manifest, dict):
        raise CheckError(f"{path} must contain a JSON object")
    return manifest


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


def _validate_exceptions(
    exceptions: object,
    table: SurfaceTable,
    matched: dict[str, Trigger],
    plugin_dir: Path,
) -> tuple[set[str], list[str]]:
    """Returns (excepted surface identifiers, problems)."""
    if exceptions is None:
        return set(), []
    if not isinstance(exceptions, dict):
        return set(), ["sdk_floor_exceptions must be a JSON object"]

    excepted: set[str] = set()
    problems: list[str] = []
    for identifier, entry in exceptions.items():
        prefix = f"sdk_floor_exceptions.{identifier}"
        surface = table.surfaces.get(identifier)
        if surface is None:
            problems.append(f"{prefix}: unknown surface identifier (not in the SDK table)")
            continue
        if not isinstance(entry, dict) or set(entry) != {"reason", "test"}:
            problems.append(f"{prefix}: entry must be an object with exactly 'reason' and 'test'")
            continue
        reason = entry["reason"]
        test_name = entry["test"]
        if not isinstance(reason, str) or not reason.strip():
            problems.append(f"{prefix}: 'reason' must be a non-empty string")
            continue
        if not isinstance(test_name, str) or "." not in test_name:
            problems.append(f"{prefix}: 'test' must name a gtest as 'Suite.Name'")
            continue
        if not surface.negotiated:
            problems.append(
                f"{prefix}: surface is not runtime-negotiated — a hard protocol "
                "requirement cannot be waived; raise min_sdk_required instead"
            )
            continue
        if identifier not in matched:
            problems.append(
                f"{prefix}: STALE — no match for this identifier in the plugin's "
                "scan closure; remove the exception"
            )
            continue
        if not _test_exists(plugin_dir, test_name):
            problems.append(
                f"{prefix}: named fallback test {test_name!r} not found in the plugin's tests"
            )
            continue
        excepted.add(identifier)
    return excepted, problems


def check_sdk_feature_floors(
    repo_root: Path = DEFAULT_REPO_ROOT,
    plugins: Sequence[Path] | None = None,
    sdk_floors_path: Path | None = None,
    *,
    out: TextIO = sys.stdout,
    err: TextIO = sys.stderr,
) -> bool:
    """Run the guardrail and return True only when every selected plugin passes."""
    repo_root = repo_root.resolve()
    if sdk_floors_path is not None and not sdk_floors_path.is_absolute():
        sdk_floors_path = repo_root / sdk_floors_path

    try:
        table = resolve_surface_table(repo_root, sdk_floors_path, err=err)
        plugin_dirs = _resolve_plugin_dirs(repo_root, plugins)
        common_graph = load_common_graph(repo_root)
    except CheckError as exc:
        print(f"ERROR: {exc}", file=err)
        return False

    failed = 0
    for plugin_dir in plugin_dirs:
        plugin_name = _display_path(plugin_dir, repo_root)
        try:
            manifest = _read_manifest(plugin_dir)
            declared = SemVer.parse(
                manifest.get("min_sdk_required"),
                f"min_sdk_required in {plugin_name}/manifest.json",
            )
            common_dirs = derive_common_closure(plugin_dir, common_graph)
            triggers = detect_triggers(repo_root, [plugin_dir, *sorted(common_dirs)], table)
        except CheckError as exc:
            failed += 1
            print(f"FAIL {plugin_name}: {exc}", file=err)
            continue

        # First trigger per surface is enough for reporting; the set is what
        # exceptions and the floor rule are validated against.
        matched: dict[str, Trigger] = {}
        for trigger in triggers:
            matched.setdefault(trigger.surface.identifier, trigger)

        excepted, problems = _validate_exceptions(
            manifest.get("sdk_floor_exceptions"), table, matched, plugin_dir
        )

        if declared < table.minimum_supported_floor:
            problems.append(
                f"min_sdk_required {declared} is below the supported floor "
                f"{table.minimum_supported_floor} (older history is not classified)"
            )

        binding = [
            trigger
            for trigger in matched.values()
            if trigger.surface.identifier not in excepted and trigger.surface.since > declared
        ]
        for trigger in sorted(binding, key=lambda item: (item.surface.since, item.surface.identifier)):
            location = f"{_display_path(trigger.path, repo_root)}:{trigger.line}"
            hint = (
                "add a validated sdk_floor_exceptions entry"
                if trigger.surface.negotiated
                else "this surface is not negotiated — the floor must rise"
            )
            problems.append(
                f"{trigger.surface.identifier} (since {trigger.surface.since}) exceeds "
                f"floor {declared} at {location}; raise min_sdk_required or {hint}"
            )

        if problems:
            failed += 1
            print(f"FAIL {plugin_name} (floor {declared}):", file=err)
            for problem in problems:
                print(f"  - {problem}", file=err)
            continue

        effective = max((t.surface.since for t in matched.values()), default=None)
        if effective is not None and declared > effective and not excepted:
            print(
                f"WARN {plugin_name}: floor {declared} is higher than any matched "
                f"surface (max {effective}) — fine if intentional",
                file=err,
            )
        summary = ", ".join(sorted(matched)) if matched else "no host surfaces matched"
        excepted_note = f"; excepted: {', '.join(sorted(excepted))}" if excepted else ""
        print(f"PASS {plugin_name} (floor {declared}): {summary}{excepted_note}", file=out)

    if failed:
        print(
            f"SDK feature-floor check failed: {failed} of {len(plugin_dirs)} plugins failed.",
            file=err,
        )
        return False
    print(f"SDK feature-floor check passed: {len(plugin_dirs)} plugins.", file=out)
    return True


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate plugin min_sdk_required declarations against used host surfaces."
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
        "--sdk-floors",
        type=Path,
        help=f"Path to the SDK's {SDK_TABLE_RELATIVE} (preferred over the interim copy)",
    )
    args = parser.parse_args(argv)
    selected_plugins = args.plugins or None
    return 0 if check_sdk_feature_floors(args.root, selected_plugins, args.sdk_floors) else 1


if __name__ == "__main__":
    raise SystemExit(main())
