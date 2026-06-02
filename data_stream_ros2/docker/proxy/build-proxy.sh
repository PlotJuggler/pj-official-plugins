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
  -DCMAKE_PREFIX_PATH="${PWD}/${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPJ_BUILD_PLUGIN=data_stream_ros2 \
  ${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}

cmake --build "${BUILD_DIR}/Release" --config Release

OUTPUT_SO="${BUILD_DIR}/Release/bin/libros2_stream_plugin.so"
echo "=== Build OK: ${OUTPUT_SO} ==="
ls -la "${OUTPUT_SO}"
