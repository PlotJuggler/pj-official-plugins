#!/usr/bin/env bash
# Make plotjuggler_sdk available in the local Conan cache without depending on a
# remote binary being available. Resolution order:
#
#   1. Already in the local cache (e.g. restored from a CI artifact) -> done.
#   2. If the PlotJuggler JFrog remote is configured, try to fetch a PREBUILT
#      binary (`--build=never`: a missing per-OS binary falls through) -> done.
#   3. Otherwise clone tag v<SDK_VERSION> from GitHub and build it via `conan create`.
#
# Single source of truth for the version: the SDK_VERSION file (exact, e.g. 0.6.0).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CORE_VERSION="${CORE_VERSION:-$(cat "${REPO_ROOT}/SDK_VERSION")}"
REF="plotjuggler_sdk/${CORE_VERSION}"
REMOTE="plotjuggler-conan"
SETTINGS=(-s build_type="${BUILD_TYPE:-Release}" -s compiler.cppstd=20)

# Use `conan cache path` (errors when the recipe is truly absent) rather than
# `conan list | grep`: conan list echoes the queried reference in its "not found"
# output, which made the grep false-positive and skip building the real package.
if conan cache path "${REF}" >/dev/null 2>&1; then
  echo "ensure_core: ${REF} already present in the local Conan cache"
  exit 0
fi

if conan remote list 2>/dev/null | grep -q "${REMOTE}"; then
  echo "ensure_core: trying prebuilt ${REF} from ${REMOTE}"
  if conan install --requires="${REF}" "${SETTINGS[@]}" \
       --build=never -r "${REMOTE}" -of "$(mktemp -d)" >/dev/null 2>&1; then
    echo "ensure_core: fetched prebuilt ${REF} from ${REMOTE}"
    exit 0
  fi
  echo "ensure_core: ${REMOTE} unavailable or has no prebuilt binary — falling back to source"
fi

echo "ensure_core: building ${REF} from source (git tag v${CORE_VERSION})"
CORE_DIR="$(mktemp -d)"
git clone --branch "v${CORE_VERSION}" --depth 1 \
  https://github.com/PlotJuggler/plotjuggler_sdk.git "${CORE_DIR}"
# Force a from-source build of core. We only reach here because JFrog has no
# matching binary (or is unavailable). The explicit `plotjuggler_sdk/*` build
# pattern builds the recipe we just exported; --build=missing covers its deps.
conan create "${CORE_DIR}" --version "${CORE_VERSION}" "${SETTINGS[@]}" \
  --build="plotjuggler_sdk/*" --build=missing
echo "ensure_core: built ${REF} from source"
