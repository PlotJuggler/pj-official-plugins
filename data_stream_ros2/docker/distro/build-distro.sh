#!/usr/bin/env bash
# Builds the data_stream_ros2 per-distro subscriber inside the ROS-aware container.
#
# Bind-mounts:
#   /workspace        pj-official-plugins source tree (required)
#   /core (ro)        plotjuggler_core checkout (optional — used by CPM to
#                     resolve the plugin's plotjuggler_core dependency)
#   /pj4              pj4 super-repo checkout (required when WITH_PJ_APP=1;
#                     mounted rw so build artifacts persist under
#                     /pj4/build/). Distinct from /core: pj4 hosts the
#                     desktop application target (pj_app) and embeds
#                     plotjuggler_core as a submodule.
#
# Outputs:
#   /workspace/build_ros2_${ROS_DISTRO}/Release/bin/libros2_stream_plugin-${ROS_DISTRO}.so
#   /pj4/build/Release/pj_app/pj_app                                              (WITH_PJ_APP=1)
#   /workspace/build_ros2_${ROS_DISTRO}/Release/test_extensions/ros2-topic-subscriber/   (WITH_PJ_APP=1)
#       └ proxy + manifest + dist/${ROS_DISTRO}/inner.so — feed the parent
#         test_extensions/ dir to pj_app's extensions directory (default
#         ~/.local/share/PlotJuggler4/extensions) via a bind-mount.

# `set -u` is intentionally omitted: ROS overlay setup scripts
# (e.g. /opt/ros/${ROS_DISTRO}/setup.bash) reference unbound vars like
# AMENT_TRACE_SETUP_FILES and abort under -u.
set -eo pipefail

: "${ROS_DISTRO:?ROS_DISTRO must be set (humble|iron|jazzy|rolling)}"

# shellcheck source=/dev/null
source "/opt/ros/${ROS_DISTRO}/setup.bash"

cd /workspace
BUILD_DIR="build_ros2_${ROS_DISTRO}"

CMAKE_EXTRA_ARGS=()
if [[ -d /core ]]; then
  CMAKE_EXTRA_ARGS+=(-DCPM_plotjuggler_core_SOURCE=/core)
elif [[ -n "${CORE_REPO_URL:-}" ]]; then
  # Redirect the canonical plotjuggler_core URL to the user-supplied override.
  # NOTE: must use --add — plain `git config key value` replaces the value, so
  # registering two insteadOf entries under the same URL key requires --add or
  # only the last one survives (and SSH/HTTPS forms stop being interchangeable).
  git config --global --add url."${CORE_REPO_URL}".insteadOf "git@github.com:PlotJuggler/plotjuggler_sdk.git"
  git config --global --add url."${CORE_REPO_URL}".insteadOf "https://github.com/PlotJuggler/plotjuggler_sdk.git"
  # Keep the pre-rename URLs too: plotjuggler_core was renamed to plotjuggler_sdk,
  # but older checkouts / GitHub redirects may still resolve the legacy name.
  git config --global --add url."${CORE_REPO_URL}".insteadOf "git@github.com:PlotJuggler/plotjuggler_core.git"
  git config --global --add url."${CORE_REPO_URL}".insteadOf "https://github.com/PlotJuggler/plotjuggler_core.git"
elif [[ -n "${SSH_AUTH_SOCK:-}" ]]; then
  # SSH agent forwarded from the host. Seed known_hosts so the clone does
  # not block on host-key confirmation, then let CMake/CPM clone via SSH.
  mkdir -p ~/.ssh
  ssh-keyscan github.com >> ~/.ssh/known_hosts 2>/dev/null
else
  # No SSH agent available — fall back to anonymous HTTPS so public mirrors
  # can still be cloned without credentials.
  git config --global url."https://github.com/".insteadOf "git@github.com:"
fi

conan profile detect --force

# Install only what data_stream_ros2 needs (plugin-local recipe), not the
# root conanfile.py which carries deps for every plugin.
conan install /workspace/data_stream_ros2 \
  --output-folder="${BUILD_DIR}" \
  --build=missing \
  -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja

cmake -S . -B "${BUILD_DIR}/Release" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${PWD}/${BUILD_DIR}/conan_toolchain.cmake" \
  -DCMAKE_PREFIX_PATH="${PWD}/${BUILD_DIR};/opt/ros/${ROS_DISTRO}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPJ_BUILD_PLUGIN=data_stream_ros2 \
  -DPJ_BUILD_ROS2_DISTRO=ON \
  ${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}

cmake --build "${BUILD_DIR}/Release" --config Release

OUTPUT_SO="${BUILD_DIR}/Release/bin/libros2_stream_plugin-${ROS_DISTRO}.so"
echo "=== Build OK: ${OUTPUT_SO} ==="
ls -la "${OUTPUT_SO}"

# ─── Optional: build pj_app from pj4 ────────────────────────────────────────
# Only runs when run-local.sh is invoked with --with-pj-app. The pj4 super-repo
# checkout is mounted rw at /pj4; build artifacts go under /pj4/build/ so they
# persist on the host. pj4's own CMakeLists.txt forces PJ_BUILD_TESTS /
# PJ_BUILD_PORTED_PLUGINS / PJ_BUILD_PARQUET_IMPORT_EXAMPLE internally, so
# this block does not pass them on the cmake command line.
if [[ "${WITH_PJ_APP:-0}" == "1" ]]; then
  [[ -d /pj4 ]] || { echo "error: --with-pj-app requires --pj4 <path>" >&2; exit 4; }
  [[ -w /pj4 ]] || { echo "error: /pj4 must be mounted read-write for --with-pj-app" >&2; exit 4; }

  PJ4_BUILD_DIR=/pj4/build
  PJ4_RELEASE_DIR="${PJ4_BUILD_DIR}/Release"

  # Toolchain mismatch guard. /pj4/build/ is shared across distros (pj4 is
  # ROS-agnostic), but Conan caches and CMake configs are tied to a specific
  # GCC. Switching between humble/iron (GCC 11) and jazzy/rolling (GCC 13)
  # on the same build dir corrupts the cache, so refuse early with a clear
  # instruction instead of trying to recover.
  #
  # Resolve symlinks before comparing — Debian/Ubuntu's update-alternatives
  # makes /usr/bin/c++ and /usr/bin/g++ both point at the same versioned
  # binary, so a textual compare would false-positive on every re-run.
  if [[ -f "${PJ4_RELEASE_DIR}/CMakeCache.txt" ]]; then
    cached_cxx="$(awk -F= '/^CMAKE_CXX_COMPILER:[A-Z]+=/{print $2; exit}' "${PJ4_RELEASE_DIR}/CMakeCache.txt" || true)"
    current_cxx="$(command -v g++)"
    if [[ -n "${cached_cxx}" && -e "${cached_cxx}" ]]; then
      cached_resolved="$(readlink -f "${cached_cxx}")"
      current_resolved="$(readlink -f "${current_cxx}")"
      if [[ "${cached_resolved}" != "${current_resolved}" ]]; then
        echo "error: ${PJ4_BUILD_DIR} was configured with ${cached_cxx} (-> ${cached_resolved})" >&2
        echo "       but the current toolchain is ${current_cxx} (-> ${current_resolved})." >&2
        echo "       Run: rm -rf ${PJ4_BUILD_DIR}" >&2
        exit 5
      fi
    fi
  fi

  echo "[build-distro] configuring pj4 (pj_app target)"
  conan install /pj4 \
    --output-folder="${PJ4_BUILD_DIR}" \
    --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=20 \
    -c tools.cmake.cmaketoolchain:generator=Ninja

  cmake -S /pj4 -B "${PJ4_RELEASE_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${PJ4_BUILD_DIR}/conan_toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="${PJ4_BUILD_DIR};${QT_DIR}" \
    -DCMAKE_BUILD_TYPE=Release

  cmake --build "${PJ4_RELEASE_DIR}" --target pj_app

  PJ_APP_BIN="${PJ4_RELEASE_DIR}/pj_app/pj_app"
  echo "=== pj_app OK: ${PJ_APP_BIN} ==="
  ls -la "${PJ_APP_BIN}"

  # ─── Assemble a single-distro test extension ──────────────────────────────
  # pj_app's ExtensionManager treats every subdirectory under its
  # extensions_dir as one installed extension and runs scanPluginDsos
  # recursively inside it. Mirror that layout so the proxy + inner pair
  # appears as a properly installed marketplace extension.
  PLUGIN_BIN_DIR="/workspace/${BUILD_DIR}/Release/bin"
  PROXY_SO="${PLUGIN_BIN_DIR}/libros2_stream_plugin.so"
  INNER_SO="${PLUGIN_BIN_DIR}/libros2_stream_plugin-${ROS_DISTRO}.so"
  EXT_MANIFEST="/workspace/data_stream_ros2/manifest.json"

  for f in "${PROXY_SO}" "${INNER_SO}" "${EXT_MANIFEST}"; do
    [[ -f "${f}" ]] || { echo "error: missing artifact for test extension: ${f}" >&2; exit 6; }
  done

  TEST_EXT_ROOT="/workspace/${BUILD_DIR}/Release/test_extensions"
  EXT_DIR="${TEST_EXT_ROOT}/ros2-topic-subscriber"
  rm -rf "${TEST_EXT_ROOT}"
  mkdir -p "${EXT_DIR}/dist/${ROS_DISTRO}"
  cp "${PROXY_SO}"     "${EXT_DIR}/"
  cp "${EXT_MANIFEST}" "${EXT_DIR}/"
  cp "${INNER_SO}"     "${EXT_DIR}/dist/${ROS_DISTRO}/"

  echo "=== Test extension assembled: ${EXT_DIR} ==="
  echo "    Bind-mount ${TEST_EXT_ROOT} as the container's extensions dir"
  echo "    (default: ~/.local/share/PlotJuggler4/extensions/) and run pj_app:"
  echo "      ${PJ_APP_BIN}"
fi
