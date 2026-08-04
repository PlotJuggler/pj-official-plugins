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

# The SDK package must match the plugins' instrumentation: an uninstrumented SDK
# linked into an instrumented plugin reports nothing for a use-after-free inside
# it. tools.build:* is acceptable here (unlike the plugin build) because this
# graph is only the SDK and its small closure.
#
# These confs do NOT participate in the Conan package_id, so the ASan lane MUST
# run with a dedicated CONAN_HOME — otherwise an instrumented package silently
# overwrites the Release one under the same id, and a later Release build links
# instrumented code (or the reverse) with no error. The app repo's AppImage
# wrapper supplies that isolation via a separate cache volume for the lane.
#
# Set before the local-SDK branch below, so `--sdk-local --asan` instruments the
# local tree too rather than registering an uninstrumented build under the pin.
if [[ "${PJ_SANITIZE:-}" == "asan" ]]; then
  SETTINGS+=(
    -c "tools.build:cxxflags=['-fsanitize=address','-fno-omit-frame-pointer']"
    -c "tools.build:cflags=['-fsanitize=address','-fno-omit-frame-pointer']"
    -c "tools.build:sharedlinkflags=['-fsanitize=address']"
    -c "tools.build:exelinkflags=['-fsanitize=address']"
  )
fi

# Local-SDK development mode (build.sh --sdk-local): register the given working
# tree in the Conan cache AS the pinned version, so every downstream plugin
# recipe resolves unchanged. Always re-created — a stale cached build of a
# changed local tree must never be silently reused. Refused in CI: the result
# is whatever the tree holds, not a reproducible release.
if [[ -n "${SDK_LOCAL_DIR:-}" ]]; then
  if [[ -n "${CI:-}" ]]; then
    echo "ensure_core: SDK_LOCAL_DIR is forbidden in CI (not reproducible)" >&2
    exit 1
  fi
  if [[ ! -f "${SDK_LOCAL_DIR}/conanfile.py" ]]; then
    echo "ensure_core: ${SDK_LOCAL_DIR} has no conanfile.py — not an SDK checkout" >&2
    exit 1
  fi
  # The SDK recipe's set_version() reads its own VERSION file and would
  # override any --version we pass — so require the tree to BE the pinned
  # version instead of pretending to relabel it.
  LOCAL_VERSION="$(tr -d '[:space:]' < "${SDK_LOCAL_DIR}/VERSION")"
  if [[ "${LOCAL_VERSION}" != "${CORE_VERSION}" ]]; then
    echo "ensure_core: local SDK VERSION ${LOCAL_VERSION} != pinned SDK_VERSION ${CORE_VERSION}" >&2
    echo "ensure_core: align them (or update the pin) before using --sdk-local" >&2
    exit 1
  fi
  echo "ensure_core: registering LOCAL tree ${SDK_LOCAL_DIR} as ${REF} (rebuilt every run)"
  conan create "${SDK_LOCAL_DIR}" "${SETTINGS[@]}" \
    --build="plotjuggler_sdk/*" --build=missing
  exit 0
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
