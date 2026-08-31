#!/usr/bin/env bash
# Upload locally-built Conan binaries to the shared PlotJuggler Artifactory so
# subsequent runs (and the release build) fetch prebuilt packages instead of
# recompiling them. The counterpart to configure_jfrog_conan.sh: only invoked
# from push / workflow_dispatch jobs that have already configured + logged into
# the remote. Pull-request jobs skip both (fork PRs receive no secrets, and we
# never publish binaries built from unreviewed code).
#
# Idempotent and cheap on warm remotes: Conan queries the remote and only
# transfers package revisions that are missing, so re-running uploads nothing
# already present. `--check` verifies manifest integrity before each transfer.
#
# Usage: upload_conan_artifacts.sh [PATTERN]
#   PATTERN defaults to "*" (every recipe + all its binaries in the local
#   cache). Pass a narrower reference (e.g. "plotjuggler_sdk/0.6.0") to scope
#   the upload to one package.
set -euo pipefail

REMOTE_NAME="plotjuggler-conan"
PATTERN="${1:-*}"

# configure_jfrog_conan.sh leaves the remote unconfigured when Artifactory is
# unreachable or credentials are absent, so "no remote" is an expected state,
# not an error. Priming the shared cache is an optimisation for later runs --
# never a reason to fail a build that has already compiled and tested cleanly.
if ! conan remote list 2>/dev/null | grep -q "${REMOTE_NAME}"; then
  echo "::warning::${REMOTE_NAME} remote is not configured; skipping upload of '${PATTERN}'"
  exit 0
fi

# The remote may be configured anonymously (read-only: pull requests, failed
# login). `conan upload` would then prompt for credentials and fail in CI, so
# only publish when configure_jfrog_conan.sh actually logged in.
if ! conan remote list-users 2>/dev/null | grep -A2 "^${REMOTE_NAME}:" | grep -qi "authenticated: *true"; then
  echo "::warning::not logged in to ${REMOTE_NAME}; skipping upload of '${PATTERN}'"
  exit 0
fi

if ! conan upload "${PATTERN}" -r "${REMOTE_NAME}" --confirm --check; then
  echo "::warning::upload of '${PATTERN}' to ${REMOTE_NAME} failed; continuing (cache priming only)"
fi
