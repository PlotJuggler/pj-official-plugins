import io
import json
import sys
from pathlib import Path

import git


SCRIPTS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_DIR))

import release_extension


def test_sdk_floor_preflight_rejects_dirty_manifest_before_scanning(
    tmp_path: Path,
    monkeypatch,
) -> None:
    repo = git.Repo.init(tmp_path)
    with repo.config_writer() as config:
        config.set_value("user", "name", "SDK Floor Test")
        config.set_value("user", "email", "sdk-floor@example.invalid")

    plugin_dir = tmp_path / "example_plugin"
    plugin_dir.mkdir()
    (plugin_dir / "manifest.json").write_text(json.dumps({"id": "example"}))
    (plugin_dir / "plugin.cpp").write_text("int plugin_entry() { return 0; }\n")
    scripts_dir = tmp_path / "scripts"
    scripts_dir.mkdir()
    (scripts_dir / "checker-input.txt").write_text("committed\n")
    common_dir = tmp_path / "common"
    common_dir.mkdir()
    (common_dir / "shared.cpp").write_text("int shared() { return 0; }\n")
    (tmp_path / "SDK_VERSION").write_text("0.21.0\n")
    repo.index.add(
        [
            "SDK_VERSION",
            "common/shared.cpp",
            "example_plugin/manifest.json",
            "example_plugin/plugin.cpp",
            "scripts/checker-input.txt",
        ]
    )
    repo.index.commit("fixture")

    # This is the exploit being guarded against: the worktree floor differs
    # from the manifest in HEAD, which is what release_extension tags.
    (plugin_dir / "manifest.json").write_text(
        json.dumps({"id": "example", "min_sdk_required": "0.21.0"})
    )
    checker_called = False

    def fake_checker(*args, **kwargs):
        nonlocal checker_called
        checker_called = True
        return True

    monkeypatch.setattr(release_extension, "check_sdk_feature_floors", fake_checker)
    stdout = io.StringIO()
    stderr = io.StringIO()

    passed = release_extension.run_sdk_feature_floor_preflight(
        repo,
        "example_plugin",
        out=stdout,
        err=stderr,
    )

    assert not passed
    assert not checker_called
    assert "differ from HEAD" in stderr.getvalue()
    assert "example_plugin/manifest.json" in stderr.getvalue()
