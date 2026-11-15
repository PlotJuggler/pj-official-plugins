#!/usr/bin/env python3
"""Decide which plugins a CI run builds, for the Qt-free lanes (ci-linux/macos/windows).

    ci_affected_plugins.py [changed paths...]

Changed paths come from the arguments; without any, a pull_request run (GITHUB_EVENT_NAME)
diffs its merge commit against its first parent (the base), and any other run selects
everything. Prints GitHub Actions outputs:

    plugins=["data_load_csv", ...]   affected plugins without Arrow (Linux matrix)
    arrow_plugins=[...]              affected Arrow consumers (Linux arrow matrix)
    aggregate=true|false             macOS: build the root aggregate
    standalone=a b ...               macOS: build each against its own recipe
    windows_aggregate, windows_standalone   the same for Windows, which also builds the
                                     aggregate when a plugin's CMake or conanfile changes:
                                     only the aggregate catches a dependency missing from
                                     the root recipe or a target name clash across plugins

A lane plugin has a manifest.json and conanfile.py, no release.sh (data_stream_ros2 has
ci-ros2.yml) and is not a widget (ci-widgets.yml). Rules per changed path:
  - ``*.md``              -> nothing (the workflows also paths-ignore markdown)
  - ``<plugin>/**``       -> that plugin, plus every plugin whose sources or CMake reach
                             into it by path (``../<plugin>/``, ``build/<plugin>/``)
  - ``common/<lib>/**``   -> every plugin with a target (tests included) that links <lib>,
                             directly or through another common/ library
  - ``common/**/CMakeLists.txt``, ``*.cmake`` -> everything (the root configures several
                             common/ libraries in every single-plugin build)
  - anything else         -> everything (root recipe, SDK_VERSION, cmake/, scripts/, ...)
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_sdk_feature_floors import (  # noqa: E402
    CheckError,
    _read_manifest,
    discover_plugin_dirs,
    load_common_graph,
    parse_cmake_model,
)

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".cmake", ".txt"}


def _lane_plugins() -> list[Path]:
    return [
        d
        for d in discover_plugin_dirs(REPO_ROOT)
        if (d / "conanfile.py").is_file()
        and not (d / "release.sh").exists()
        and _read_manifest(d).get("category") != "widget"
    ]


def _links_common(plugin_dir: Path, common_targets: set[str]) -> bool:
    """True when any target in the plugin's tree (tests included) links one of common_targets.

    Unresolved link expressions are ignored: check_sdk_feature_floors already rejects them on
    release targets, and the only test-side ones are toolbox_mosaico's Arrow target probes.
    Failing closed there would build mosaico (Arrow Flight) for every common/ change; a link
    hidden behind a variable in a test still gets built by the full run on main."""
    for path in plugin_dir.rglob("CMakeLists.txt"):
        try:
            model = parse_cmake_model(path)
        except CheckError:
            return True  # cannot tell, so build it rather than crash the scope job
        if model.dynamic_link_dependencies & common_targets:
            return True
        if any(deps & common_targets for deps in model.edges.values()):
            return True
    return False


def _path_dependents(plugin_dirs: list[Path]) -> dict[str, set[str]]:
    """plugin -> the plugins whose sources or CMake name it by path (e.g. a test including
    ``../toolbox_assistant_agent/tests/support/...``, or loading ``build/data_load_parquet/...``)."""
    names = {d.name for d in plugin_dirs}
    pattern = re.compile(r"(?:\.\./|/)(" + "|".join(map(re.escape, sorted(names))) + r")/")
    dependents: dict[str, set[str]] = {}
    for d in plugin_dirs:
        for path in d.rglob("*"):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                for target in pattern.findall(path.read_text(encoding="utf-8", errors="replace")):
                    if target != d.name:
                        dependents.setdefault(target, set()).add(d.name)
    return dependents


def affected_plugins(changed: list[str] | None) -> tuple[bool, list[str]]:
    """(everything, affected lane plugin names); changed=None selects everything."""
    lane = {d.name: d for d in _lane_plugins()}
    every = sorted(lane)
    if changed is None:
        return True, every
    plugin_dirs = discover_plugin_dirs(REPO_ROOT)
    plugin_names = {d.name for d in plugin_dirs}
    common_libs = {p.parent.name for p in (REPO_ROOT / "common").glob("*/CMakeLists.txt")}

    selected: set[str] = set()
    touched_libs: set[str] = set()
    for path in changed:
        parts = Path(path.strip()).parts
        if not parts or path.strip().endswith(".md"):
            continue
        if len(parts) > 1 and parts[0] in plugin_names:
            selected.add(parts[0])
        elif (
            len(parts) > 2
            and parts[0] == "common"
            and parts[1] in common_libs
            and parts[-1] != "CMakeLists.txt"
            and not parts[-1].endswith(".cmake")
        ):
            touched_libs.add(parts[1])
        else:
            return True, every

    if touched_libs:
        graph = load_common_graph(REPO_ROOT)
        # The touched libraries' targets, plus every common target linking them transitively.
        hit = {t for t, d in graph.target_dirs.items() if d.name in touched_libs}
        while grown := {s for s, deps in graph.edges.items() if s in graph.target_dirs and deps & hit} - hit:
            hit |= grown
        selected |= {name for name, d in lane.items() if _links_common(d, hit)}

    if selected:
        dependents = _path_dependents(plugin_dirs)
        pending = list(selected)
        while pending:
            for dependent in dependents.get(pending.pop(), ()):
                if dependent not in selected:
                    selected.add(dependent)
                    pending.append(dependent)

    affected = sorted(selected & lane.keys())
    return affected == every, affected


def _recipe_edited(changed: list[str] | None) -> bool:
    """A plugin's CMake or conanfile changed: something only the aggregate can check."""
    return changed is None or any(
        Path(p).name in ("CMakeLists.txt", "conanfile.py") or p.endswith(".cmake") for p in changed
    )


def build_plan(changed: list[str] | None) -> dict[str, str]:
    everything, affected = affected_plugins(changed)
    is_arrow = {p: '"arrow/' in (REPO_ROOT / p / "conanfile.py").read_text(encoding="utf-8") for p in affected}
    lane = affected_plugins(None)[1]
    root = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    outside_root = [p for p in lane if p not in set(re.findall(r"add_subdirectory\((\w+)\)", root))]

    def plan(aggregate: bool) -> tuple[str, str]:
        # With the aggregate, only the plugins it leaves out build on their own.
        return ("true", " ".join(outside_root)) if aggregate else ("false", " ".join(affected))

    # ponytail: plugins build one by one, each with its own conan install, so past a third
    # of the lane the single aggregate build is cheaper. Measure before tuning the ratio.
    broad = everything or 3 * len(affected) >= len(lane)
    aggregate, standalone = plan(broad)
    windows_aggregate, windows_standalone = plan(broad or (bool(affected) and _recipe_edited(changed)))
    return {
        "plugins": json.dumps([p for p in affected if not is_arrow[p]]),
        "arrow_plugins": json.dumps([p for p in affected if is_arrow[p]]),
        "aggregate": aggregate,
        "standalone": standalone,
        "windows_aggregate": windows_aggregate,
        "windows_standalone": windows_standalone,
    }


def pr_changed_files(repo: Path) -> list[str]:
    """Files a pull request changes: its merge commit against the first parent (the base).
    --no-renames lists a moved file at both paths, so the side it left is selected too."""
    diff = ["git", "-C", str(repo), "-c", "core.quotePath=false", "diff", "--no-renames", "--name-only"]
    return subprocess.run([*diff, "HEAD^1", "HEAD"], check=True, capture_output=True, text=True).stdout.splitlines()


def main(argv: list[str]) -> int:
    if argv:
        changed: list[str] | None = argv
    elif os.environ.get("GITHUB_EVENT_NAME") == "pull_request":
        changed = pr_changed_files(REPO_ROOT)
    else:
        changed = None
    for key, value in build_plan(changed).items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
