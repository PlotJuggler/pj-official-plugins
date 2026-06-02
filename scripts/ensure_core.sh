#!/usr/bin/env bash
# Make plotjuggler_sdk available in the local Conan cache WITHOUT depending on the
# cloudsmith remote at build time. Resolution order:
#
#   1. Already in the local cache (e.g. restored from a CI artifact) -> done.
#   2. If the cloudsmith remote is configured, try to fetch a PREBUILT binary
#      (`--build=never`: never build from the remote recipe, so a 402 / bandwidth
#      failure or a missing per-OS binary simply falls through) -> done on success.
#   3. Otherwise build from the pinned git submodule (no network) via `conan create`.
#
# Single source of truth for the version: the SDK_VERSION file (exact, e.g. 0.6.0),
# which must equal the submodule's pinned tag v<SDK_VERSION>.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CORE_VERSION="${CORE_VERSION:-$(cat "${REPO_ROOT}/SDK_VERSION")}"
REF="plotjuggler_sdk/${CORE_VERSION}"
REMOTE="plotjuggler-cloudsmith"
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

echo "ensure_core: building ${REF} from source (extern/plotjuggler_core)"
CORE_DIR="${REPO_ROOT}/extern/plotjuggler_core"
if [ ! -f "${CORE_DIR}/conanfile.py" ]; then
  # A pinned, non-tip commit cannot be fetched with `--depth 1`, so do a full
  # submodule fetch (non-fatal); the direct tag clone below is the last resort.
  git -C "${REPO_ROOT}" submodule update --init --recursive extern/plotjuggler_core || true
fi
if [ ! -f "${CORE_DIR}/conanfile.py" ]; then
  echo "ensure_core: submodule not populated — cloning v${CORE_VERSION} directly"
  rm -rf "${CORE_DIR}"
  git clone --branch "v${CORE_VERSION}" --depth 1 \
    https://github.com/PlotJuggler/plotjuggler_sdk.git "${CORE_DIR}"
fi
# Force a from-source build of core. We only reach here because the cloudsmith
# binary is unavailable (e.g. 402), but if the remote is still configured a plain
# --build=missing sees the advertised (unservable) binary and tries to DOWNLOAD it,
# failing again with 402. The explicit `plotjuggler_sdk/*` build pattern overrides
# that and builds from the recipe we just exported; --build=missing covers core's
# own dependencies.
conan create "${CORE_DIR}" --version "${CORE_VERSION}" "${SETTINGS[@]}" \
  --build="plotjuggler_sdk/*" --build=missing
echo "ensure_core: built ${REF} from source"
