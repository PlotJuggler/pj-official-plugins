#!/usr/bin/env bash
# Configure the shared PlotJuggler Artifactory repository ahead of ConanCenter.
#
# The repository allows anonymous READ, so the remote is useful to every job:
# without credentials (pull requests, fork PRs, local use) it is added
# anonymously and serves prebuilt binaries; with JFROG_USER / JFROG_TOKEN
# (push / tag jobs) the script also logs in so upload_conan_artifacts.sh can
# publish. Keep credentials scoped to the workflow step that calls this script.
#
# The remote is a build ACCELERATOR, not a correctness requirement: everything
# it serves is also resolvable from ConanCenter, and ensure_core.sh falls back
# to building the SDK from the extern/plotjuggler_core submodule. So an
# unreachable Artifactory degrades to slower builds instead of failing the job
# -- a third-party outage must not be able to take down every CI leg and every
# release at once. Problems are reported as ::warning:: so they stay visible in
# the run summary rather than passing unnoticed.
set -euo pipefail

REMOTE_NAME="plotjuggler-conan"
REMOTE_URL="https://plotjuggler.jfrog.io/artifactory/api/conan/plotjuggler-conan"

# Never leave a half-configured remote behind: an added-but-unauthenticated
# remote makes every later `conan install` query a server that cannot serve it,
# which is slower and noisier than having no remote at all.
drop_remote() {
  conan remote remove "${REMOTE_NAME}" >/dev/null 2>&1 || true
}

if ! conan remote add "${REMOTE_NAME}" "${REMOTE_URL}" --force --index=0; then
  echo "::warning::could not add ${REMOTE_NAME}; resolving from ConanCenter instead"
  drop_remote
  exit 0
fi

if [[ -z "${JFROG_USER:-}" || -z "${JFROG_TOKEN:-}" ]]; then
  echo "::notice::JFROG_USER / JFROG_TOKEN are unset; using ${REMOTE_NAME} anonymously (read-only)"
elif ! conan remote login "${REMOTE_NAME}" "${JFROG_USER}" -p "${JFROG_TOKEN}"; then
  # Anonymous read still works, so keep the remote for downloads; only the
  # upload step (which checks for a logged-in user) will be skipped.
  echo "::warning::login to ${REMOTE_NAME} failed (invalid credentials, or Artifactory unavailable); continuing anonymously (read-only)"
  conan remote logout "${REMOTE_NAME}" >/dev/null 2>&1 || true
fi

first_remote="$(conan remote list 2>/dev/null | sed -n '1s/:.*//p' || true)"
if [[ "${first_remote}" != "${REMOTE_NAME}" ]]; then
  echo "::warning::${REMOTE_NAME} is not ordered before ConanCenter (found ${first_remote:-no remote} first); continuing with degraded cache hits"
fi

conan remote list
