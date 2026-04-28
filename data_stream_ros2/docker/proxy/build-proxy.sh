#!/usr/bin/env bash
# Builds the data_stream_ros2 proxy inside the plain Ubuntu container.
#
# Bind-mounts:
#   /workspace        pj-official-plugins source tree (required)
#   /core (ro)        plotjuggler_core checkout (optional — if absent, CPM
#                     fetches the public source over the network)
#
# Output: /workspace/build_ros2_proxy/Release/bin/libros2_stream_plugin.so

set -eo pipefail

cd /workspace
BUILD_DIR="build_ros2_proxy"

CMAKE_EXTRA_ARGS=()
if [[ -d /core ]]; then
  CMAKE_EXTRA_ARGS+=(-DCPM_plotjuggler_core_SOURCE=/core)
elif [[ -n "${CORE_REPO_URL:-}" ]]; then
  # Redirect the canonical plotjuggler_core URL to the user-supplied override.
  git config --global url."${CORE_REPO_URL}".insteadOf "git@github.com:PlotJuggler/plotjuggler_core.git"
  git config --global url."${CORE_REPO_URL}".insteadOf "https://github.com/PlotJuggler/plotjuggler_core.git"
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
  -DCMAKE_PREFIX_PATH="${PWD}/${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPJ_BUILD_PLUGIN=data_stream_ros2 \
  ${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}

cmake --build "${BUILD_DIR}/Release" --config Release

OUTPUT_SO="${BUILD_DIR}/Release/bin/libros2_stream_plugin.so"
echo "=== Build OK: ${OUTPUT_SO} ==="
ls -la "${OUTPUT_SO}"
