#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"

usage() {
  cat <<EOF
Usage: ./build.sh [--help] [--sdk-local[=path]] [--asan|--tsan] [plugin_dir]

Build all plugins:
  ./build.sh

Build one plugin:
  ./build.sh data_load_csv

Develop against an UNRELEASED SDK (no Conan release needed):
  ./build.sh --sdk-local data_load_mcap
      Registers the local SDK working tree (default:
      ~/ws_plotjuggler/plotjuggler_sdk, override with --sdk-local=/path) in
      the Conan cache AS the pinned SDK_VERSION, rebuilding it every run.
      NOT reproducible — never use for release artifacts; refused in CI.

Sanitizer lanes (compiled with Clang; see "Sanitizer lanes" in CLAUDE.md):
  ./build.sh --asan [plugin_dir]   AddressSanitizer: instrumented plugins, tests not built
  ./build.sh --tsan [plugin_dir]   ThreadSanitizer: instrumented plugins, tests built and run

Environment:
  BUILD_TYPE=${BUILD_TYPE}  CMake/Conan build type
  PJ_SANITIZE=asan|tsan  same as --asan / --tsan
  PJ_SANITIZER_CLANG_VERSION=${PJ_SANITIZER_CLANG_VERSION:-22}  Clang major version for the sanitizer lanes (CC/CXX override it)
EOF
}

SDK_LOCAL_DIR=""
# --asan / --tsan instrument only the plugins' own targets. Conan's tools.build:* config is not
# used because it would rebuild the whole dependency graph (Arrow, Flight, gRPC, protobuf)
# under the sanitizer. The SDK is the exception (instrumented in scripts/ensure_core.sh): an
# uninstrumented SDK linked into an instrumented plugin reports nothing.
SANITIZE="${PJ_SANITIZE:-}"
while [[ "${1:-}" == -* ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --asan)
      SANITIZE=asan
      shift
      ;;
    --tsan)
      SANITIZE=tsan
      shift
      ;;
    --sdk-local)
      SDK_LOCAL_DIR="$HOME/ws_plotjuggler/plotjuggler_sdk"
      shift
      ;;
    --sdk-local=*)
      SDK_LOCAL_DIR="${1#--sdk-local=}"
      shift
      ;;
    *)
      usage >&2
      exit 1
      ;;
  esac
done

if [[ "$#" -gt 1 ]]; then
  usage >&2
  exit 1
fi

# ensure_core.sh reads PJ_SANITIZE to instrument the SDK, including a --sdk-local tree.
if [[ -n "$SANITIZE" ]]; then
  export PJ_SANITIZE="$SANITIZE"
fi
# shellcheck source=scripts/sanitizer_compiler.sh
source "$SCRIPT_DIR/scripts/sanitizer_compiler.sh"
pj_select_sanitizer_compiler "$SANITIZE"

if [[ -n "$SDK_LOCAL_DIR" ]]; then
  if [[ ! -d "$SDK_LOCAL_DIR" ]]; then
    echo "Error: --sdk-local directory not found: $SDK_LOCAL_DIR" >&2
    exit 1
  fi
  SDK_LOCAL_DIR="$(cd "$SDK_LOCAL_DIR" && pwd)"
  export SDK_LOCAL_DIR
  echo "=============================================================================="
  echo " SDK from local tree $SDK_LOCAL_DIR"
  echo " registered as plotjuggler_sdk/$(cat "$SCRIPT_DIR/SDK_VERSION") — NOT reproducible;"
  echo " do not use for release artifacts."
  echo "=============================================================================="
  "$SCRIPT_DIR/scripts/ensure_core.sh"
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
  # Lane trees nest under build/ so the app repo's AppImage builder (recursive chown of build/)
  # reaches them.
  BUILD_DIR="${BUILD_DIR/\/build\//\/build\/asan\/}"
  CMAKE_BUILD_DIR="$BUILD_DIR/$BUILD_TYPE"
  # -g is explicit: this repo has no debug-info policy, and an ASan report without file and line is
  # not actionable.
  CMAKE_ARGS+=(
    "-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
    "-DCMAKE_C_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address"
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address"
    # Instrumented codegen yields false-positive warnings; see PJ_PLUGINS_NO_WERROR in the root
    # CMakeLists.txt.
    "-DPJ_PLUGINS_NO_WERROR=ON"
    # This lane only produces plugin .so files for an instrumented AppImage; ASan plus -g makes
    # every test executable large and nothing runs them.
    "-DBUILD_TESTING=OFF"
  )
  echo "Sanitizer: AddressSanitizer (plugin targets only; Conan deps unchanged)"
fi

if [[ "$SANITIZE" == "tsan" ]]; then
  BUILD_DIR="${BUILD_DIR/\/build\//\/build\/tsan\/}"
  CMAKE_BUILD_DIR="$BUILD_DIR/$BUILD_TYPE"
  CMAKE_ARGS+=(
    "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -g"
    "-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -g"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread"
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"
    "-DPJ_PLUGINS_NO_WERROR=ON"
    # TSan reports races only in code that runs, so this lane must build and run the tests.
    "-DBUILD_TESTING=ON"
  )
  echo "Sanitizer: ThreadSanitizer (plugin targets only; Conan deps unchanged)"
fi

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

if [[ "$IS_WINDOWS" != true ]]; then
  # liblsl 1.16.2 vendors asio 1.20.0, which falls back to `std::result_of` (removed in C++20, and
  # absent from libc++) unless ASIO_HAS_STD_INVOKE_RESULT is defined; its autodetection only covers
  # MSVC. Not applied on Windows: liblsl's CMakeLists forces CMAKE_CXX_STANDARD 14 under MSVC only,
  # where std::invoke_result does not exist yet, so defining the macro there fails with C2143/C2059
  # at type_traits.hpp:86. liblsl cannot be bumped (ConanCenter has 1.16.2 at most).
  CONAN_ARGS+=("-c" 'liblsl/*:tools.build:defines=["ASIO_HAS_STD_INVOKE_RESULT=1"]')
fi

echo "Conan recipe: $CONAN_RECIPE"
echo "Build directory: $CMAKE_BUILD_DIR"

# -s:b compiler.cppstd=20: the root recipe pulls protobuf into the build context (tool_requires,
# for protoc). protobuf 6.x requires C++17 and a default build profile (e.g. MSVC cppstd=14) fails
# its validate(); host -s does not cover the build context.
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

if [[ "$SANITIZE" == "tsan" ]]; then
  # ThreadSanitizer needs narrower mmap randomisation than current kernels use
  # (vm.mmap_rnd_bits=32); GCC's runtime aborts with "unexpected memory mapping", Clang's
  # re-executes itself without randomisation. `setarch -R` disables it for the test process without
  # a host sysctl change, but needs the personality syscall that Docker's default seccomp profile
  # denies (the app repo's container wrapper relaxes it for this lane). If refused, run unwrapped
  # and let the runtime report rather than pass without executing anything.
  tsan_launch=()
  if setarch -R true >/dev/null 2>&1; then
    tsan_launch=(setarch -R)
  else
    echo "warning: 'setarch -R' unavailable; ThreadSanitizer may abort before running tests." >&2
  fi
  # Clang's sanitizer runtimes symbolize only through an llvm-symbolizer on PATH.
  symbolizer_path=""
  if [[ -x "/usr/lib/llvm-${PJ_SANITIZER_CLANG_VERSION:-22}/bin/llvm-symbolizer" ]]; then
    symbolizer_path="/usr/lib/llvm-${PJ_SANITIZER_CLANG_VERSION:-22}/bin:"
  fi
  PATH="${symbolizer_path}${PATH}" \
  QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
  TSAN_OPTIONS="halt_on_error=1 history_size=4 ${TSAN_OPTIONS:-}" \
    ${tsan_launch[@]+"${tsan_launch[@]}"} \
    ctest --test-dir "$CMAKE_BUILD_DIR" --output-on-failure --timeout 120
fi
