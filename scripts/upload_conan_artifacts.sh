#!/usr/bin/env bash
# Upload locally-built Conan binaries to the shared PlotJuggler Artifactory so later runs fetch
# prebuilt packages. Counterpart to configure_jfrog_conan.sh: only invoked from push /
# workflow_dispatch jobs that already configured + logged into the remote. Pull-request jobs skip
# both (fork PRs get no secrets, and binaries built from unreviewed code are never published).
#
# Idempotent: Conan transfers only missing package revisions; `--check` verifies manifest integrity.
#
# Usage: upload_conan_artifacts.sh [PATTERN]
#   PATTERN defaults to "*" (every recipe + binary in the local cache); pass a narrower reference
#   (e.g. "plotjuggler_sdk/0.6.0") to scope the upload.
set -euo pipefail

REMOTE_NAME="plotjuggler-conan"
PATTERN="${1:-*}"

# configure_jfrog_conan.sh leaves the remote unconfigured when Artifactory is unreachable, so "no
# remote" is expected; priming the cache must never fail an already-green build.
if ! conan remote list 2>/dev/null | grep -q "${REMOTE_NAME}"; then
  echo "::warning::${REMOTE_NAME} remote is not configured; skipping upload of '${PATTERN}'"
  exit 0
fi

# An anonymous (read-only) remote would make `conan upload` prompt for credentials and fail in CI,
# so only publish when configure_jfrog_conan.sh actually logged in.
if ! conan remote list-users 2>/dev/null | grep -A2 "^${REMOTE_NAME}:" | grep -qi "authenticated: *true"; then
  echo "::warning::not logged in to ${REMOTE_NAME}; skipping upload of '${PATTERN}'"
  exit 0
fi

if ! conan upload "${PATTERN}" -r "${REMOTE_NAME}" --confirm --check; then
  echo "::warning::upload of '${PATTERN}' to ${REMOTE_NAME} failed; continuing (cache priming only)"
fi
