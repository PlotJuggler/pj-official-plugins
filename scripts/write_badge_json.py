#!/usr/bin/env python3
"""Write a shields.io endpoint-badge JSON for one plugin.

The badge encodes two facts that a native GitHub Actions badge cannot carry at
once: the plugin's released *version* (from its ``manifest.json``) and whether
its CI job *passed* (encoded as the badge color). shields.io renders the JSON
via its ``endpoint`` schema:

    https://img.shields.io/endpoint?url=<raw-url-to-this-json>

See https://shields.io/badges/endpoint-badge for the schema.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# GitHub ``job.status`` / aggregated result -> shields color.
COLOR_BY_STATUS = {
    "success": "brightgreen",
    "failure": "red",
    "cancelled": "lightgrey",
    "skipped": "lightgrey",
}


def read_version(plugin_dir: Path) -> str:
    """Return the ``version`` field from ``<plugin_dir>/manifest.json``.

    Returns ``"unknown"`` (and warns) if the manifest is missing or malformed,
    so a manifest glitch downgrades the badge message rather than failing CI.
    """
    manifest = plugin_dir / "manifest.json"
    try:
        version = json.loads(manifest.read_text())["version"]
    except (FileNotFoundError, KeyError, json.JSONDecodeError) as err:
        print(f"warning: cannot read version from {manifest}: {err}", file=sys.stderr)
        return "unknown"
    return str(version)


def build_payload(plugin: str, status: str, version: str) -> dict:
    color = COLOR_BY_STATUS.get(status, "lightgrey")
    message = f"v{version}" if version != "unknown" else "unknown"
    return {
        "schemaVersion": 1,
        "label": plugin,
        "message": message,
        "color": color,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin", required=True, help="plugin directory name")
    parser.add_argument(
        "--status",
        required=True,
        help="GitHub job.status or aggregated result: success|failure|cancelled|skipped",
    )
    parser.add_argument("--out-dir", required=True, help="directory to write <plugin>.json into")
    args = parser.parse_args()

    payload = build_payload(args.plugin, args.status, read_version(Path(args.plugin)))

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / f"{args.plugin}.json"
    out_file.write_text(json.dumps(payload) + "\n")

    print(f"wrote {out_file}: {json.dumps(payload)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
