#!/usr/bin/env bash
# Release entry point for data_stream_ros2, used by .github/workflows/build-release.yml as the plugin's build override
# (see scripts/release_tools.py resolve-build-scope).
#
# Builds the proxy + every per-distro inner via Docker, then stages the assembled marketplace tree under
# <build_dir>/dist/, which release_tools.py create-distribution-package packages as-is (staging-dir mode).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUNDLE_DIR="${REPO_ROOT}/dist_ros2"
STAGING_DIR="${REPO_ROOT}/build/data_stream_ros2/Release/dist"

# plotjuggler_core is private. The SSH agent from the workflow's "Configure
# SSH for private dependencies" step is forwarded into the build containers
# by docker/run-local.sh (when SSH_AUTH_SOCK is set in the environment),
# matching the same CORE_DEPLOY_KEY-based access the rest of the plugins use.

"${SCRIPT_DIR}/docker/run-local.sh" --bundle

# docker/run-local.sh writes <repo>/dist_ros2/; the workflow expects the tree under the build directory.
rm -rf "${STAGING_DIR}"
mkdir -p "$(dirname "${STAGING_DIR}")"
mv "${BUNDLE_DIR}" "${STAGING_DIR}"

echo "=== Staged at ${STAGING_DIR} ==="
find "${STAGING_DIR}" -type f | sort
