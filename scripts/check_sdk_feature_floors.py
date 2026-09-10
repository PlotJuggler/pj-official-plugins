#!/usr/bin/env python3
"""Enforce each plugin's ``min_sdk_required`` against the host surfaces it uses.

Repo driver over the SDK's reusable floor-checker core
(``scripts/vendor/feature_floor_check.py``, a temporary byte-copy of the module
the SDK ships next to ``feature_floors.json`` from 0.33.0 — see
``scripts/vendor/README.md``). The core owns the table schema, source
scanning, exception validation, and the floor rule; this driver owns what is
specific to this monorepo:

  - the CMake link closure: each plugin's scan set is its directory plus every
    ``common/`` library reachable from its ``pj_emit_plugin_manifest`` targets
    through ``target_link_libraries`` (unresolved link expressions fail closed);
  - iteration over every top-level ``manifest.json`` plugin with aggregated
    PASS/WARN/FAIL reporting;
  - table resolution: an explicit ``--sdk-floors`` path wins (and must match
    the interim copies byte-for-payload); otherwise the bundled interim table
    stands in until the pinned ``SDK_VERSION`` ships it. Once ``SDK_VERSION``
    reaches 0.33.0 the check HARD-FAILS while interim artifacts still exist,
    forcing the cutover instead of trusting a doc comment.

Known, deliberate boundaries: the scan set is the LINK closure's directories —
include-only dependencies, sources added via ``target_sources()`` from outside
those directories, and generated code are not covered (linking is the repo's
dependency contract; a Clang compile-command-based extractor is the upgrade
path). And only RUNTIME host-contract surfaces are visible: compile-time SDK
library APIs (``parseIso8601Utc``, ``sliderToWindow``, ...) never appear here,
so "no host surfaces matched" or an over-declared-floor WARNING is NEVER by
itself justification to lower ``min_sdk_required`` — toolbox_mosaico and
data_load_mp4 (both 0.31.0 for compile-time reasons) are the canonical
examples, correct as declared. A ``floor_test`` proves the DEGRADED PATH at
the finest testable granularity: helper-level is acceptable when the full
entry point needs a live host, and the test comment must then state the
routing fact.

Per plugin, the lean floor contract (see the core module): the floor must
cover every matched non-negotiated surface; a matched surface above the floor
requires ``suggested_sdk_version`` (== max since over all matched surfaces)
plus an existing ``floor_test`` gtest, both forbidden otherwise. Floors below
the table's ``minimum_supported_floor`` fail; over-declared floors only warn
(the maintainer may know a runtime reason the scan cannot see).
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence, TextIO

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR / "vendor"))
import feature_floor_check as core  # noqa: E402
from feature_floor_check import CheckError  # noqa: E402

DEFAULT_REPO_ROOT = SCRIPT_DIR.parent
INTERIM_TABLE_NAME = "sdk_feature_floors_interim.json"
VENDORED_CORE_NAME = "feature_floor_check.py"
SDK_TABLE_RELATIVE = Path("pj_base") / "feature_floors.json"
# The release whose SDK package ships table + checker; the interim copies must
# be deleted (and CI rewired to the SDK's own files) when SDK_VERSION reaches it.
SDK_SHIPS_TABLE_FROM = (0, 33, 0)

EXCLUDED_SOURCE_DIRS = frozenset(
    {
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
)
TEST_SOURCE_DIRS = ("tests", "test")
# CMake variables that are genuinely external (never targets under common/).
# Kept explicit so a newly introduced unresolved expression fails closed
# instead of silently shrinking a plugin's source closure.
EXTERNAL_LINK_EXPRESSION_ALLOWLIST = {
    "${CMAKE_DL_LIBS}": "CMake's platform dynamic-loader libraries",
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


@dataclass(frozen=True)
class Trigger:
    identifier: str
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


# --- Table resolution --------------------------------------------------------


def _surfaces_payload(path: Path) -> object:
    document = json.loads(path.read_text(encoding="utf-8"))
    return {
        "minimum_supported_floor": document.get("minimum_supported_floor"),
        "since": document.get("since"),
        "baseline": document.get("baseline", []),
    }


def _repo_sdk_version(repo_root: Path) -> tuple[int, int, int] | None:
    version_file = repo_root / "SDK_VERSION"
    if not version_file.is_file():
        return None
    return core.parse_version(version_file.read_text().strip(), "SDK_VERSION file")


def resolve_surface_table(
    repo_root: Path,
    sdk_floors_path: Path | None,
    *,
    err: TextIO,
) -> core.SurfaceTable:
    """Prefer the SDK's own table; fall back to the bundled interim copy."""
    interim_path = repo_root / "scripts" / INTERIM_TABLE_NAME
    vendored_core_path = repo_root / "scripts" / "vendor" / VENDORED_CORE_NAME

    sdk_version = _repo_sdk_version(repo_root)
    if sdk_version is not None and sdk_version >= SDK_SHIPS_TABLE_FROM:
        leftovers = [path for path in (interim_path, vendored_core_path) if path.is_file()]
        if leftovers:
            listed = ", ".join(str(path) for path in leftovers)
            raise CheckError(
                f"SDK_VERSION {core.format_version(sdk_version)} ships the floor table and "
                f"checker in the package — delete the interim artifacts ({listed}) and point "
                "the check at the SDK's share/plotjuggler_sdk/ copies via --sdk-floors"
            )

    if sdk_floors_path is not None:
        if not sdk_floors_path.is_file():
            raise CheckError(f"--sdk-floors table not found: {sdk_floors_path}")
        table = core.load_surface_table(sdk_floors_path)
        # A diverging interim artifact is a rot hazard: the SDK copy wins, and
        # the stale duplicate fails the check outright (sync or delete it).
        if interim_path.is_file():
            if _surfaces_payload(sdk_floors_path) != _surfaces_payload(interim_path):
                raise CheckError(
                    f"interim table {interim_path} differs from the SDK table "
                    f"{sdk_floors_path}; sync it or delete it (the SDK copy wins)"
                )
            print(f"NOTE: SDK surface table in use; delete the interim copy {interim_path}", file=err)
        if vendored_core_path.is_file():
            # Installed layout: core beside the table. SDK checkout layout:
            # table under pj_base/, core under tools/feature_floors/.
            candidates = [
                sdk_floors_path.parent / VENDORED_CORE_NAME,
                sdk_floors_path.parent.parent / "tools" / "feature_floors" / VENDORED_CORE_NAME,
            ]
            sdk_core_path = next((path for path in candidates if path.is_file()), None)
            if sdk_core_path is None:
                raise CheckError(
                    f"cannot verify the vendored checker core: no {VENDORED_CORE_NAME} found "
                    f"beside {sdk_floors_path} or in its tools/feature_floors/ layout"
                )
            if sdk_core_path.read_bytes() != vendored_core_path.read_bytes():
                raise CheckError(
                    f"vendored checker core {vendored_core_path} differs from the SDK's "
                    f"{sdk_core_path}; sync it or delete scripts/vendor/ (the SDK copy wins)"
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
    return core.load_surface_table(interim_path)


# --- CMake link-closure resolution -------------------------------------------


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

    variable_match = re.fullmatch(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", token)
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
        # An empty CMake value (`set(X "")`) means "no dependency", not an
        # unresolvable expression — filtering it lets ordinary optional-dep
        # variables resolve through the machinery instead of the allowlist.
        if name == "set" and tokens and COMMAND_NAME_RE.fullmatch(tokens[0]):
            variables.setdefault(tokens[0], set()).update(token for token in tokens[1:] if token)
        elif (
            name == "list"
            and len(tokens) >= 2
            and tokens[0].upper() in {"APPEND", "PREPEND"}
            and COMMAND_NAME_RE.fullmatch(tokens[1])
        ):
            variables.setdefault(tokens[1], set()).update(token for token in tokens[2:] if token)

    for name, tokens in commands:
        if not tokens:
            continue
        if name == "add_library":
            if not LITERAL_TARGET_RE.fullmatch(tokens[0]):
                # A variable-named library target must resolve, or the file's
                # closure would silently lose it — fail closed instead.
                resolved_names, name_unresolved = _resolve_link_item(tokens[0], variables, path)
                if name_unresolved or not resolved_names:
                    raise CheckError(
                        f"add_library target {tokens[0]!r} in {path} cannot be resolved; "
                        "declare it via set() or use a literal name"
                    )
                model.targets.update(resolved_names)
                continue
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


# --- Per-plugin scan and verdict ---------------------------------------------


def _display_path(path: Path, repo_root: Path) -> str:
    try:
        return path.relative_to(repo_root).as_posix()
    except ValueError:
        return str(path)


def detect_triggers(
    scan_dirs: Sequence[Path],
    table: core.SurfaceTable,
    file_cache: dict[Path, list[tuple[str, int]]],
) -> list[Trigger]:
    """Matches across the scan set; per-file results are memoized for the run
    so a common/ library shared by many plugins is scanned once, not per
    consumer."""
    triggers: list[Trigger] = []
    source_files = sorted(
        {
            path
            for directory in scan_dirs
            for path in core.iter_source_files(directory, EXCLUDED_SOURCE_DIRS)
        }
    )
    for path in source_files:
        if path not in file_cache:
            file_cache[path] = core.scan_source_file(path, table)
        triggers.extend(Trigger(identifier, path, line) for identifier, line in file_cache[path])
    return triggers


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

    file_cache: dict[Path, list[tuple[str, int]]] = {}
    failed = 0
    for plugin_dir in plugin_dirs:
        plugin_name = _display_path(plugin_dir, repo_root)
        try:
            manifest = _read_manifest(plugin_dir)
            declared = core.parse_version(
                manifest.get("min_sdk_required"),
                f"min_sdk_required in {plugin_name}/manifest.json",
            )
            common_dirs = derive_common_closure(plugin_dir, common_graph)
            triggers = detect_triggers([plugin_dir, *sorted(common_dirs)], table, file_cache)
        except CheckError as exc:
            failed += 1
            print(f"FAIL {plugin_name}: {exc}", file=err)
            continue

        # First trigger per surface is enough for reporting; the mapping is
        # what exceptions and the floor rule are validated against.
        matched: dict[str, str] = {}
        for trigger in sorted(
            triggers, key=lambda item: (_display_path(item.path, repo_root), item.line)
        ):
            matched.setdefault(
                trigger.identifier, f"{_display_path(trigger.path, repo_root)}:{trigger.line}"
            )

        test_dirs = [plugin_dir / name for name in TEST_SOURCE_DIRS]
        problems = core.floor_problems(manifest, declared, table, matched, test_dirs)

        if problems:
            failed += 1
            print(f"FAIL {plugin_name} (floor {core.format_version(declared)}):", file=err)
            for problem in problems:
                print(f"  - {problem}", file=err)
            continue

        effective = core.effective_maximum(table, matched)
        if effective is not None and declared > effective:
            print(
                f"WARN {plugin_name}: floor {core.format_version(declared)} is higher than any "
                f"matched surface (max {core.format_version(effective)}) — may be intentional "
                "(compile-time SDK APIs are outside this check)",
                file=err,
            )
        summary = ", ".join(sorted(matched)) if matched else "no host surfaces matched"
        suggested = manifest.get("suggested_sdk_version")
        degraded_note = f", full features at {suggested}" if suggested else ""
        print(
            f"PASS {plugin_name} (floor {core.format_version(declared)}{degraded_note}): {summary}",
            file=out,
        )

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
