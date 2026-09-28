#!/usr/bin/env bash
# Build the RoboStack payload of data_stream_ros2 for one distro:
#
#   data_stream_ros2/robostack/build.sh <jazzy|kilted> [output-dir]
#
# Output: <output-dir>/libros2_stream_plugin-<distro>.pjros2 (default
# build_ros2_<distro>_robostack/). The proxy ships it as
# dist/<distro>-robostack/ and loads it when PlotJuggler runs from a conda
# environment that holds rclcpp.
#
# The payload carries no RPATH/RUNPATH on purpose. Its ROS libraries then
# resolve through the DT_RPATH of the conda-installed plotjuggler4 executable
# ($ORIGIN/../lib, i.e. the environment's lib/): the environment's path is
# unknown at build time, and a RUNPATH would stop glibc from consulting the
# executable's.
set -euo pipefail

DISTRO="${1:?usage: build.sh <jazzy|kilted> [output-dir]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
OUT="${2:-${REPO}/build_ros2_${DISTRO}_robostack}"
WORK="${REPO}/build_ros2_${DISTRO}_robostack_work"
SDK_VERSION="$(tr -d '[:space:]' < "${REPO}/SDK_VERSION")"

pixi run --manifest-path "${HERE}/pixi.toml" -e "${DISTRO}" bash -euo pipefail -c '
  distro="$1"; repo="$2"; work="$3"; out="$4"; sdk="$5"
  mkdir -p "${work}" "${out}"

  # plotjuggler_sdk from its pinned release, installed into the work dir.
  # Warnings stay warnings: this consumes a released SDK, and newer GCCs flag
  # false positives in it (-Wfree-nonheap-object at -O3).
  if [[ ! -f "${work}/sdk-install/lib/cmake/plotjuggler_sdk/plotjuggler_sdkConfig.cmake" ]]; then
    curl -fsSL "https://github.com/PlotJuggler/plotjuggler_sdk/archive/refs/tags/v${sdk}.tar.gz" \
      | tar -xz -C "${work}"
    cmake -G Ninja -S "${work}/plotjuggler_sdk-${sdk}" -B "${work}/sdk-build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_PREFIX_PATH="${CONDA_PREFIX}" -DCMAKE_INSTALL_PREFIX="${work}/sdk-install" \
      -DPJ_INSTALL_SDK=ON -DPJ_BUILD_TESTS=OFF -DPJ_BUILD_PORTED_PLUGINS=OFF \
      -DPJ_WARNINGS_AS_ERRORS=OFF
    cmake --build "${work}/sdk-build"
    cmake --install "${work}/sdk-build"
  fi

  rm -rf "${work}/plugin-build"
  cmake -G Ninja -S "${repo}" -B "${work}/plugin-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${work}/sdk-install;${CONDA_PREFIX}" \
    -DPJ_BUILD_PLUGIN=data_stream_ros2 -DPJ_BUILD_ROS2_DISTRO=ON -DROS_DISTRO="${distro}"
  cmake --build "${work}/plugin-build" --target ros2_stream_plugin_distro

  cp "${work}/plugin-build/bin/libros2_stream_plugin-${distro}.so" \
    "${out}/libros2_stream_plugin-${distro}.pjros2"
  # The conda toolchain bakes the build env into every link (see the header).
  patchelf --remove-rpath "${out}/libros2_stream_plugin-${distro}.pjros2"
' _ "${DISTRO}" "${REPO}" "${WORK}" "${OUT}" "${SDK_VERSION}"

payload="${OUT}/libros2_stream_plugin-${DISTRO}.pjros2"
if readelf -d "${payload}" | grep -qE "RPATH|RUNPATH"; then
  echo "error: ${payload} carries an RPATH/RUNPATH" >&2
  exit 1
fi
echo "=== Build OK: ${payload} ==="
