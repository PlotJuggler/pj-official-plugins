#!/usr/bin/env bash
# Builds the data_stream_ros2 per-distro subscriber inside the ROS-aware container.
#
# Bind-mounts:
#   /workspace        pj-official-plugins source tree (required)
#   /core (ro)        plotjuggler_core checkout (optional — if absent, CPM
#                     fetches the public source over the network)
#
# Output: /workspace/build_ros2_${ROS_DISTRO}/Release/bin/libros2_stream_plugin-${ROS_DISTRO}.so

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
else
  # Parent CMake fetches plotjuggler_core via SSH. Without an SSH agent in
  # the container, fall back to HTTPS so the clone succeeds anonymously.
  git config --global url."https://github.com/".insteadOf "git@github.com:"
fi

conan profile detect --force

conan install . \
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
