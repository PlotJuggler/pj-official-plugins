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

if ! conan remote list 2>/dev/null | grep -q "${REMOTE_NAME}"; then
  echo "::error::${REMOTE_NAME} remote is not configured; run configure_jfrog_conan.sh first" >&2
  exit 1
fi

conan upload "${PATTERN}" -r "${REMOTE_NAME}" --confirm --check
