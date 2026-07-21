#!/usr/bin/env bash
# Configure the shared PlotJuggler Artifactory repository ahead of ConanCenter.
#
# Keep credentials scoped to the workflow step that calls this script. In
# particular, pull-request jobs do not call it because fork PRs do not receive
# repository or organization secrets.
set -euo pipefail

: "${JFROG_USER:?Set JFROG_USER as a GitHub Actions variable or secret}"
: "${JFROG_TOKEN:?Set JFROG_TOKEN as a GitHub Actions secret}"

REMOTE_NAME="plotjuggler-conan"
REMOTE_URL="https://plotjuggler.jfrog.io/artifactory/api/conan/plotjuggler-conan"

conan remote add "${REMOTE_NAME}" "${REMOTE_URL}" --force --index=0
conan remote login "${REMOTE_NAME}" "${JFROG_USER}" -p "${JFROG_TOKEN}"

first_remote="$(conan remote list | sed -n '1s/:.*//p')"
if [[ "${first_remote}" != "${REMOTE_NAME}" ]]; then
  echo "::error::${REMOTE_NAME} must be ordered before ConanCenter (found ${first_remote:-no remote} first)" >&2
  exit 1
fi

conan remote list
