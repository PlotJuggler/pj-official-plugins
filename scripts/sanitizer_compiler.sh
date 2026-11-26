#!/usr/bin/env bash
# Compiler selection for the asan/tsan lanes. Sourced by build.sh and scripts/ensure_core.sh, which
# must agree: the SDK and the plugins linking it have to be instrumented for the same compiler's
# sanitizer runtime (code instrumented for one runtime cannot load into a process running another).
#
# Compiles with Clang PJ_SANITIZER_CLANG_VERSION (default 22, apt.llvm.org: clang-<v>,
# libclang-rt-<v>-dev, llvm-<v>), matching PlotJuggler 4's instrumented lanes. A CC/CXX the caller
# already set is kept. The Conan profile is left alone so prebuilt dependency binaries keep their
# GCC package ids.

# pj_select_sanitizer_compiler <lane>
#   Exports CC/CXX (and PJ_SANITIZER_CLANG_VERSION) for asan/tsan, no-op otherwise; returns 1 when
#   the required Clang is missing.
pj_select_sanitizer_compiler() {
  local lane="${1:-}"
  case "$lane" in
    asan|tsan) ;;
    *) return 0 ;;
  esac
  export PJ_SANITIZER_CLANG_VERSION="${PJ_SANITIZER_CLANG_VERSION:-22}"
  if [[ -z "${CXX:-}" ]]; then
    local version="$PJ_SANITIZER_CLANG_VERSION"
    if ! command -v "clang++-${version}" >/dev/null 2>&1; then
      echo "Error: the ${lane} lane compiles with clang++-${version}, which is not installed." >&2
      echo "       Install clang-${version}, libclang-rt-${version}-dev and llvm-${version} from apt.llvm.org," >&2
      echo "       or set CC/CXX to choose the compiler yourself." >&2
      return 1
    fi
    export CC="clang-${version}" CXX="clang++-${version}"
  fi
  echo "Sanitizer lane ${lane}: compiling with ${CXX}"
}
