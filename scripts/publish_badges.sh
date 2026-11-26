#!/usr/bin/env bash
#
# Publish per-plugin status badge JSON to the orphan `badges` branch.
#
# Each CI workflow funnels its plugins' badge JSON (from write_badge_json.py) into one directory and
# calls this script, which commits the files onto a source-free `badges` branch that shields.io
# reads via raw.githubusercontent.com.
#
# Several workflows write disjoint files but can still race on `git push`; the retry loop resyncs
# to the remote tip and reapplies our JSON so concurrent writers converge.
#
# Required environment:
#   GITHUB_REPOSITORY  owner/repo (provided by Actions)
#   GH_TOKEN           token with contents:write on the repo
#
# Usage: publish_badges.sh <dir-of-json-files>

set -euo pipefail

SRC_DIR="${1:?usage: publish_badges.sh <dir-of-json-files>}"
if [[ ! -d "${SRC_DIR}" ]]; then
  echo "no badge directory '${SRC_DIR}' (no artifacts); nothing to publish"
  exit 0
fi
SRC_DIR="$(cd "${SRC_DIR}" && pwd)"
BRANCH="badges"
COMMIT_MSG="chore(badges): update plugin status [skip ci]"

# BADGES_REMOTE_URL overrides the authenticated GitHub URL (local tests point it at a scratch bare repo).
if [[ -n "${BADGES_REMOTE_URL:-}" ]]; then
  REMOTE_URL="${BADGES_REMOTE_URL}"
else
  : "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"
  : "${GH_TOKEN:?GH_TOKEN must be set}"
  REMOTE_URL="https://x-access-token:${GH_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
fi

shopt -s nullglob
json_files=("${SRC_DIR}"/*.json)
if [[ ${#json_files[@]} -eq 0 ]]; then
  echo "no badge JSON in ${SRC_DIR}; nothing to publish"
  exit 0
fi
echo "publishing ${#json_files[@]} badge file(s) from ${SRC_DIR}"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT
cd "${work_dir}"

git init -q
git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
git remote add origin "${REMOTE_URL}"

if git fetch -q --depth=1 origin "${BRANCH}"; then
  git checkout -q -B "${BRANCH}" FETCH_HEAD
else
  echo "branch '${BRANCH}' not found; creating it as an orphan"
  git checkout -q --orphan "${BRANCH}"
fi

stage_and_commit() {
  cp "${SRC_DIR}"/*.json .
  git add -A
  if git diff --cached --quiet; then
    return 1 # nothing changed
  fi
  git commit -q -m "${COMMIT_MSG}"
  return 0
}

if ! stage_and_commit; then
  echo "badge content already up to date; nothing to push"
  exit 0
fi

for attempt in 1 2 3 4 5; do
  if git push -q origin "HEAD:${BRANCH}"; then
    echo "badges pushed (attempt ${attempt})"
    exit 0
  fi
  echo "push rejected; resyncing to remote tip (attempt ${attempt})"
  git fetch -q --depth=1 origin "${BRANCH}"
  git reset -q --hard FETCH_HEAD
  if ! stage_and_commit; then
    echo "remote already carries our badge content"
    exit 0
  fi
done

echo "ERROR: could not push badges after retries" >&2
exit 1
