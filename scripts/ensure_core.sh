#!/usr/bin/env bash
# Make plotjuggler_sdk available in the local Conan cache without depending on a
# remote binary being available. Resolution order:
#
#   1. Already in the local cache (e.g. restored from a CI artifact) -> done.
#   2. If the PlotJuggler JFrog remote is configured, try to fetch a PREBUILT
#      binary (`--build=never`: a missing per-OS binary falls through) -> done.
#   3. Otherwise build from the pinned git submodule (no network) via `conan create`.
#
# Single source of truth for the version: the SDK_VERSION file (exact, e.g. 0.6.0),
# which must equal the submodule's pinned tag v<SDK_VERSION>.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CORE_VERSION="${CORE_VERSION:-$(cat "${REPO_ROOT}/SDK_VERSION")}"
REF="plotjuggler_sdk/${CORE_VERSION}"
REMOTE="plotjuggler-conan"
SETTINGS=(-s build_type="${BUILD_TYPE:-Release}" -s compiler.cppstd=20)

# The SDK package must match the plugins' instrumentation. tools.build:* is
# acceptable here (unlike the plugin build) because this graph is only the SDK
# and its small closure. NOTE: these confs do NOT participate in the Conan
# package_id, so the ASan lane MUST run with a dedicated CONAN_HOME — otherwise
# an instrumented package silently overwrites the Release one under the same id.
# appimage/build_in_docker.sh --asan supplies that via a separate cache volume.
if [[ "${PJ_SANITIZE:-}" == "asan" ]]; then
  SETTINGS+=(
    -c "tools.build:cxxflags=['-fsanitize=address','-fno-omit-frame-pointer']"
    -c "tools.build:cflags=['-fsanitize=address','-fno-omit-frame-pointer']"
    -c "tools.build:sharedlinkflags=['-fsanitize=address']"
    -c "tools.build:exelinkflags=['-fsanitize=address']"
  )
fi

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
# Force a from-source build of core. We only reach here because JFrog has no
# matching binary (or is unavailable). The explicit `plotjuggler_sdk/*` build
# pattern builds the recipe we just exported; --build=missing covers its deps.
conan create "${CORE_DIR}" --version "${CORE_VERSION}" "${SETTINGS[@]}" \
  --build="plotjuggler_sdk/*" --build=missing
echo "ensure_core: built ${REF} from source"
