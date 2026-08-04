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

# --asan (or PJ_SANITIZE=asan) instruments the plugins' OWN targets with
# AddressSanitizer. Deliberately NOT injected through Conan's tools.build:*
# config: that applies to the whole dependency graph, which would rebuild
# Arrow + Flight + gRPC + protobuf under ASan with a fresh package_id — hours of
# work for code we are not hunting in. Prebuilt Conan deps stay as-is.
SANITIZE="${PJ_SANITIZE:-}"
if [[ "${1:-}" == "--asan" ]]; then
  SANITIZE=asan
  shift
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

if [[ "$SANITIZE" == "asan" ]]; then
  BUILD_DIR="${BUILD_DIR/\/build\//\/build-asan\/}"
  CMAKE_BUILD_DIR="$BUILD_DIR/$BUILD_TYPE"
  CMAKE_ARGS+=(
    "-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
    "-DCMAKE_C_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address"
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address"
  )
  echo "Sanitizer: AddressSanitizer (plugin targets only; Conan deps unchanged)"
fi

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

# -s:b compiler.cppstd=20: the aggregate root recipe pulls protobuf into the
# BUILD context (tool_requires, for the Conan protoc). protobuf 6.x requires
# C++17, but a default build profile (e.g. MSVC's cppstd=14) fails its validate()
# with "Current cppstd is lower than the required C++ standard". Host -s only
# covers the host context, so the build context needs its own cppstd.
#
# liblsl/*:...ASIO_HAS_STD_INVOKE_RESULT: liblsl 1.16.2 vendors asio 1.20.0, whose
# detail/type_traits.hpp falls back to `using std::result_of;` unless that macro is
# defined -- and its autodetection (detail/config.hpp) only implements the MSVC
# branch, none for clang or gcc. std::result_of was removed in C++20: MSVC takes the
# good branch, libstdc++ still ships it (deprecated) so Linux compiles by accident,
# and libc++ actually removed it, so macOS fails to build liblsl at all. Defining the
# macro is exactly what asio itself does for MSVC, and keeps the whole graph on C++20
# (dropping just liblsl to C++17 would straddle a standard boundary through Boost,
# which liblsl links). Bumping liblsl is not an option: ConanCenter has 1.16.2 at most.
conan install "$CONAN_RECIPE" --output-folder="$BUILD_DIR" --build=missing \
  -s build_type="$BUILD_TYPE" \
  -s compiler.cppstd=20 \
  -s:b compiler.cppstd=20 \
  -c 'liblsl/*:tools.build:defines=["ASIO_HAS_STD_INVOKE_RESULT=1"]' \
  -c tools.cmake.cmaketoolchain:generator=Ninja \
  ${CONAN_ARGS[@]+"${CONAN_ARGS[@]}"}

cmake -S "$SCRIPT_DIR" -B "$CMAKE_BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
  -DCMAKE_PREFIX_PATH="$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  ${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}

cmake --build "$CMAKE_BUILD_DIR" --config "$BUILD_TYPE" --parallel
