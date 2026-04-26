#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"

usage() {
  cat <<EOF
Usage: ./build.sh [--help] [plugin_dir]

Build all plugins:
  ./build.sh

Build one plugin:
  ./build.sh data_load_csv

Environment:
  BUILD_TYPE=${BUILD_TYPE}  CMake/Conan build type
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "$#" -gt 1 || "${1:-}" == -* ]]; then
  usage >&2
  exit 1
fi

PLUGIN="${1:-}"
CMAKE_ARGS=()

if [[ -n "$PLUGIN" ]]; then
  if [[ ! -d "$SCRIPT_DIR/$PLUGIN" ]]; then
    echo "Error: plugin directory not found: $PLUGIN" >&2
    exit 1
  fi
  if [[ ! -f "$SCRIPT_DIR/$PLUGIN/conanfile.py" ]]; then
    echo "Error: plugin Conan recipe not found: $PLUGIN/conanfile.py" >&2
    exit 1
  fi

  CONAN_RECIPE="$SCRIPT_DIR/$PLUGIN"
  BUILD_DIR="$SCRIPT_DIR/build/$PLUGIN"
  CMAKE_ARGS+=("-DPJ_BUILD_PLUGIN=$PLUGIN")
else
  CONAN_RECIPE="$SCRIPT_DIR"
  BUILD_DIR="$SCRIPT_DIR/build/all"
fi

CMAKE_BUILD_DIR="$BUILD_DIR/$BUILD_TYPE"
CONAN_ARGS=()

if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
  CONAN_ARGS+=("-o" "cpython/*:shared=True")
else
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
      CONAN_ARGS+=("-o" "cpython/*:shared=True")
      ;;
  esac
fi

echo "Conan recipe: $CONAN_RECIPE"
echo "Build directory: $CMAKE_BUILD_DIR"

conan install "$CONAN_RECIPE" --output-folder="$BUILD_DIR" --build=missing \
  -s build_type="$BUILD_TYPE" \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja \
  ${CONAN_ARGS[@]+"${CONAN_ARGS[@]}"}

cmake -S "$SCRIPT_DIR" -B "$CMAKE_BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
  -DCMAKE_PREFIX_PATH="$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  ${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}

cmake --build "$CMAKE_BUILD_DIR" --config "$BUILD_TYPE" --parallel
