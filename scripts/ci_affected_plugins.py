#!/usr/bin/env python3
"""Map a PR's changed files to the plugins CI must build.

Reads changed paths (one per line) on stdin -- or nothing with ``--all`` -- and prints
GitHub Actions outputs for the Qt-free lanes (ci-linux/macos/windows):

    all=true|false                  true: build everything (the aggregate on macOS/Windows)
    plugins=["data_load_csv", ...]  the affected lane plugins, JSON
    list=data_load_csv ...          the same, space-separated, for plain bash

A lane plugin has a conanfile.py, no release.sh (data_stream_ros2: ci-ros2.yml) and is
not a widget (ci-widgets.yml); those two workflows keep their own path filters.

Rules, first match wins per file:
  - ``*.md``                         -> nothing (the workflows also paths-ignore them)
  - ``<plugin>/**``                  -> that plugin (a top-level dir with manifest.json)
  - ``common/<lib>/**``              -> every plugin whose CMake link closure reaches <lib>
                                        (same closure check_sdk_feature_floors.py scans)
  - anything else (root recipe, SDK_VERSION, cmake/, build.sh, scripts/, workflows,
    loose common/*.hpp, tools/, ...) -> all plugins

Unknown paths fail closed to "all"; pushes to main build everything anyway.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_sdk_feature_floors import derive_common_closure, discover_plugin_dirs, load_common_graph  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent


def _in_lane(plugin_dir: Path) -> bool:
    manifest = json.loads((plugin_dir / "manifest.json").read_text(encoding="utf-8"))
    return (
        (plugin_dir / "conanfile.py").is_file()
        and not (plugin_dir / "release.sh").exists()
        and manifest.get("category") != "widget"
    )


def affected_plugins(changed: list[str], repo_root: Path = REPO_ROOT) -> tuple[bool, list[str]]:
    plugin_dirs = discover_plugin_dirs(repo_root)
    names = {p.name for p in plugin_dirs}
    every = sorted(p.name for p in plugin_dirs if _in_lane(p))
    common_libs = {p.parent.name for p in (repo_root / "common").glob("*/CMakeLists.txt")}

    selected: set[str] = set()
    touched_libs: set[str] = set()
    for path in changed:
        parts = Path(path.strip()).parts
        if not parts or path.endswith(".md"):
            continue
        if len(parts) > 1 and parts[0] in names:
            selected.add(parts[0])
        elif len(parts) > 2 and parts[0] == "common" and parts[1] in common_libs:
            touched_libs.add(parts[1])
        else:
            return True, every

    if touched_libs:
        graph = load_common_graph(repo_root)
        # The release closure is transitive but skips test executables, so also match any
        # direct mention of the library's targets in the plugin's CMake files (pj_test_support).
        targets = [t for t, d in graph.target_dirs.items() if d.name in touched_libs]
        mention = re.compile(r"(?<![\w:])(" + "|".join(map(re.escape, targets)) + r")(?![\w])") if targets else None
        for plugin_dir in plugin_dirs:
            cmake_text = "\n".join(p.read_text(encoding="utf-8") for p in plugin_dir.rglob("CMakeLists.txt"))
            if {d.name for d in derive_common_closure(plugin_dir, graph)} & touched_libs or (
                mention and mention.search(cmake_text)
            ):
                selected.add(plugin_dir.name)

    selected_list = sorted(selected & set(every))
    return selected_list == every, selected_list


def main() -> int:
    if sys.argv[1:] == ["--all"]:
        all_, plugins = affected_plugins(["--all"])  # an unknown path selects everything
    else:
        all_, plugins = affected_plugins([line for line in sys.stdin.read().splitlines() if line.strip()])
    print(f"all={'true' if all_ else 'false'}")
    print(f"plugins={json.dumps(plugins)}")
    print(f"list={' '.join(plugins)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
