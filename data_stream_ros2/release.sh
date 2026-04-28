#!/usr/bin/env bash
# Release entry point for data_stream_ros2.
#
# Picked up by .github/workflows/build-release.yml when this plugin's tag is
# pushed (the workflow honours <plugin>/release.sh as a build override —
# see scripts/release_tools.py resolve-build-scope).
#
# Builds the proxy + every per-distro inner via Docker, then stages the
# pre-assembled marketplace tree under <build_dir>/dist/ where
# release_tools.py create-distribution-package picks it up via staging-dir
# mode and packages it as-is.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUNDLE_DIR="${REPO_ROOT}/dist_ros2"
STAGING_DIR="${REPO_ROOT}/build/data_stream_ros2/Release/dist"

# plotjuggler_core is private. The SSH agent from the workflow's "Configure
# SSH for private dependencies" step is forwarded into the build containers
# by docker/run-local.sh (when SSH_AUTH_SOCK is set in the environment),
# matching the same CORE_DEPLOY_KEY-based access the rest of the plugins use.

# Build proxy + per-distro inner + assemble marketplace tree.
"${SCRIPT_DIR}/docker/run-local.sh" --bundle

# Hand the assembled tree to create-distribution-package via the staging-dir
# convention (<build_dir>/dist/). docker/run-local.sh writes to
# <repo>/dist_ros2/; relocate it under the build directory the workflow
# expects.
rm -rf "${STAGING_DIR}"
mkdir -p "$(dirname "${STAGING_DIR}")"
mv "${BUNDLE_DIR}" "${STAGING_DIR}"

echo "=== Staged at ${STAGING_DIR} ==="
find "${STAGING_DIR}" -type f | sort
