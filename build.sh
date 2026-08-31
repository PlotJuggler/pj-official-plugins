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

IS_WINDOWS=false
if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
  IS_WINDOWS=true
else
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
      IS_WINDOWS=true
      ;;
  esac
fi

if [[ "$IS_WINDOWS" == true ]]; then
  CONAN_ARGS+=("-o" "cpython/*:shared=True")
else
  # liblsl 1.16.2 vendors asio 1.20.0, whose detail/type_traits.hpp falls back to
  # `using std::result_of;` unless ASIO_HAS_STD_INVOKE_RESULT is defined -- and its
  # autodetection in detail/config.hpp only implements the MSVC branch, none for
  # clang or gcc. std::result_of was removed in C++20, and libc++ actually dropped
  # it, so without this macOS cannot build liblsl at all.
  #
  # Everywhere-but-Windows, and that asymmetry is not arbitrary. liblsl's own
  # CMakeLists.txt has:
  #
  #     if(NOT MSVC_VERSION VERSION_LESS 1700)
  #       set(CMAKE_CXX_STANDARD 14)
  #     endif()
  #
  # MSVC_VERSION is empty on non-MSVC compilers, so "" VERSION_LESS 1700 is true and
  # the NOT makes it false: the set() runs *only* under MSVC, overriding the C++20 the
  # toolchain asked for. So MSVC compiles liblsl as C++14 -- where std::result_of still
  # exists and std::invoke_result does not yet -- while clang and gcc get C++20, where
  # it is the other way round. Defining the macro on MSVC forces the invoke_result
  # branch in a C++14 translation unit and fails with C2143/C2059 at type_traits.hpp:86.
  #
  # Bumping liblsl is not an option: ConanCenter has 1.16.2 at most.
  CONAN_ARGS+=("-c" 'liblsl/*:tools.build:defines=["ASIO_HAS_STD_INVOKE_RESULT=1"]')
fi

echo "Conan recipe: $CONAN_RECIPE"
echo "Build directory: $CMAKE_BUILD_DIR"

# -s:b compiler.cppstd=20: the aggregate root recipe pulls protobuf into the
# BUILD context (tool_requires, for the Conan protoc). protobuf 6.x requires
# C++17, but a default build profile (e.g. MSVC's cppstd=14) fails its validate()
# with "Current cppstd is lower than the required C++ standard". Host -s only
# covers the host context, so the build context needs its own cppstd.
conan install "$CONAN_RECIPE" --output-folder="$BUILD_DIR" --build=missing \
  -s build_type="$BUILD_TYPE" \
  -s compiler.cppstd=20 \
  -s:b compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja \
  ${CONAN_ARGS[@]+"${CONAN_ARGS[@]}"}

cmake -S "$SCRIPT_DIR" -B "$CMAKE_BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
  -DCMAKE_PREFIX_PATH="$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  ${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}

cmake --build "$CMAKE_BUILD_DIR" --config "$BUILD_TYPE" --parallel
