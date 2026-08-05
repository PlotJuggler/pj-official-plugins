import io
import json
import sys
from pathlib import Path

import pytest


SCRIPTS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_DIR))

import check_sdk_feature_floors as checker


DEFAULT_FEATURES = {
    "NewSdkCall(": "0.21.0",
    "NewerSdkCall(": "0.22.0",
}


def write_floors(root: Path, features: dict[str, str] | None = None) -> Path:
    path = root / "scripts" / "sdk_feature_floors.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "matching_rule": "cpp_token_v1",
                "_comment": "test fixture",
                "features": features or DEFAULT_FEATURES,
            }
        )
    )
    return path


def write_plugin(
    root: Path,
    *,
    source: str = "int plugin_entry() { return 0; }\n",
    min_sdk_required: str | None = None,
    links: tuple[str, ...] = (),
) -> Path:
    plugin_dir = root / "example_plugin"
    plugin_dir.mkdir()
    manifest = {"id": "example"}
    if min_sdk_required is not None:
        manifest["min_sdk_required"] = min_sdk_required
    (plugin_dir / "manifest.json").write_text(json.dumps(manifest))
    (plugin_dir / "plugin.cpp").write_text(source)
    link_line = ""
    if links:
        link_line = (
            "target_link_libraries(example_plugin PRIVATE " + " ".join(links) + ")\n"
        )
    (plugin_dir / "CMakeLists.txt").write_text(
        "add_library(example_plugin SHARED plugin.cpp)\n"
        + link_line
        + "pj_emit_plugin_manifest(example_plugin MANIFEST_FILE manifest.json)\n"
    )
    return plugin_dir


def make_repo(tmp_path: Path, sdk_version: str = "0.22.0") -> tuple[Path, Path]:
    (tmp_path / "SDK_VERSION").write_text(sdk_version + "\n")
    floors_path = write_floors(tmp_path)
    return tmp_path, floors_path


def run_check(
    root: Path,
    plugin_dir: Path,
    floors_path: Path,
) -> tuple[bool, str, str]:
    stdout = io.StringIO()
    stderr = io.StringIO()
    result = checker.check_sdk_feature_floors(
        root,
        [plugin_dir],
        floors_path,
        out=stdout,
        err=stderr,
    )
    return result, stdout.getvalue(), stderr.getvalue()


def test_no_usage_passes_without_manifest_floor(tmp_path: Path) -> None:
    root, floors_path = make_repo(tmp_path)
    plugin_dir = write_plugin(root)

    passed, stdout, stderr = run_check(root, plugin_dir, floors_path)

    assert passed
    assert "no SDK feature usage detected" in stdout
    assert stderr == ""


def test_mapped_identifier_in_comment_does_not_raise_floor(tmp_path: Path) -> None:
    root, floors_path = make_repo(tmp_path)
    plugin_dir = write_plugin(root, source="// Future example: NewSdkCall()\n")

    passed, stdout, stderr = run_check(root, plugin_dir, floors_path)

    assert passed
    assert "no SDK feature usage detected" in stdout
    assert stderr == ""


@pytest.mark.parametrize("manifest_floor", [None, "0.20.0"])
def test_direct_usage_fails_for_absent_or_lower_manifest_floor(
    tmp_path: Path,
    manifest_floor: str | None,
) -> None:
    root, floors_path = make_repo(tmp_path)
    plugin_dir = write_plugin(
        root,
        source="void use_sdk() { NewSdkCall(); }\n",
        min_sdk_required=manifest_floor,
    )

    passed, _, stderr = run_check(root, plugin_dir, floors_path)

    assert not passed
    assert "effective SDK floor 0.21.0" in stderr
    assert "identifier 'NewSdkCall('" in stderr
    assert "example_plugin/plugin.cpp:1" in stderr


def test_direct_usage_passes_with_matching_manifest_floor(tmp_path: Path) -> None:
    root, floors_path = make_repo(tmp_path)
    plugin_dir = write_plugin(
        root,
        source="void use_sdk() { NewSdkCall(); }\n",
        min_sdk_required="0.21.0",
    )

    passed, stdout, stderr = run_check(root, plugin_dir, floors_path)

    assert passed
    assert "effective SDK floor 0.21.0 <= min_sdk_required 0.21.0" in stdout
    assert stderr == ""


def test_usage_through_transitive_common_library_fails(tmp_path: Path) -> None:
    root, floors_path = make_repo(tmp_path)
    leaf_dir = root / "common" / "leaf"
    bridge_dir = root / "common" / "bridge"
    leaf_dir.mkdir(parents=True)
    bridge_dir.mkdir(parents=True)
    (leaf_dir / "CMakeLists.txt").write_text("add_library(pj_leaf INTERFACE)\n")
    (leaf_dir / "leaf.hpp").write_text("inline void helper() { NewSdkCall(); }\n")
    (bridge_dir / "CMakeLists.txt").write_text(
        "add_library(pj_bridge INTERFACE)\n"
        "target_link_libraries(pj_bridge INTERFACE pj_leaf)\n"
    )
    (bridge_dir / "bridge.hpp").write_text("inline void bridge() {}\n")
    plugin_dir = write_plugin(root, links=("pj_bridge",))

    passed, _, stderr = run_check(root, plugin_dir, floors_path)

    assert not passed
    assert "common/leaf/leaf.hpp:1" in stderr
    assert "identifier 'NewSdkCall('" in stderr


@pytest.mark.parametrize(
    "source",
    [
        "// PJ_REQUIRE_SDK_VERSION(0, 22, 0)\nint plugin_entry();\n",
        "PJ_REQUIRE_SDK_VERSION(0, 22, 0)\nint plugin_entry();\n",
    ],
)
def test_annotation_in_code_or_comment_raises_effective_floor(
    tmp_path: Path,
    source: str,
) -> None:
    root, floors_path = make_repo(tmp_path)
    plugin_dir = write_plugin(
        root,
        source=source,
        min_sdk_required="0.21.0",
    )

    passed, _, stderr = run_check(root, plugin_dir, floors_path)

    assert not passed
    assert "effective SDK floor 0.22.0" in stderr
    assert "annotation 'PJ_REQUIRE_SDK_VERSION(0, 22, 0)'" in stderr


def test_annotation_cannot_lower_detected_floor(tmp_path: Path) -> None:
    root, floors_path = make_repo(tmp_path)
    plugin_dir = write_plugin(
        root,
        source=(
            "// PJ_REQUIRE_SDK_VERSION(0, 21, 0)\n"
            "void use_sdk() { NewerSdkCall(); }\n"
        ),
        min_sdk_required="0.21.0",
    )

    passed, _, stderr = run_check(root, plugin_dir, floors_path)

    assert not passed
    assert "effective SDK floor 0.22.0" in stderr
    assert "identifier 'NewerSdkCall('" in stderr
    assert "annotation 'PJ_REQUIRE_SDK_VERSION(0, 21, 0)'" in stderr


def test_manifest_newer_than_pinned_sdk_fails_without_usage(tmp_path: Path) -> None:
    root, floors_path = make_repo(tmp_path, sdk_version="0.21.0")
    plugin_dir = write_plugin(root, min_sdk_required="0.22.0")

    passed, _, stderr = run_check(root, plugin_dir, floors_path)

    assert not passed
    assert (
        "manifest min_sdk_required 0.22.0 is newer than pinned SDK_VERSION 0.21.0"
        in stderr
    )


def test_semver_comparison_honors_prerelease_precedence() -> None:
    parse = lambda value: checker.SemVer.parse(value, "test version")

    assert parse("0.21.0-beta.2") < parse("0.21.0-beta.11")
    assert parse("0.21.0-beta.11") < parse("0.21.0-rc.1")
    assert parse("0.21.0-rc.1") < parse("0.21.0")
    assert parse("0.21.0+build.1") == parse("0.21.0+build.2")


@pytest.mark.parametrize(
    "malformed",
    [
        "{ not-json",
        json.dumps(
            {
                "schema_version": 1,
                "matching_rule": "cpp_token_v1",
                "features": {"NewSdkCall(": "next"},
            }
        ),
    ],
)
def test_malformed_floor_map_fails_loudly(tmp_path: Path, malformed: str) -> None:
    root, floors_path = make_repo(tmp_path)
    plugin_dir = write_plugin(root)
    floors_path.write_text(malformed)

    passed, _, stderr = run_check(root, plugin_dir, floors_path)

    assert not passed
    assert stderr.startswith("ERROR:")
    assert "feature-floor map" in stderr or "minimum SDK version" in stderr
