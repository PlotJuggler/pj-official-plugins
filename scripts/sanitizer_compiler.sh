#!/usr/bin/env bash
# Compiler selection for the instrumented lanes. Sourced by build.sh and
# scripts/ensure_core.sh, which must agree: the SDK and the plugins that link it
# have to be instrumented for the same compiler's sanitizer runtime, and code
# instrumented for one runtime cannot be loaded into a process running another.
#
# The asan and tsan lanes compile with Clang PJ_SANITIZER_CLANG_VERSION (default
# 22, from apt.llvm.org: clang-<v>, libclang-rt-<v>-dev, llvm-<v>). That is the
# compiler PlotJuggler 4's instrumented lanes use, and its container builds this
# repository the same way. A compiler the caller already chose through CC/CXX is
# kept. The Conan profile is left alone, so prebuilt dependency binaries keep their
# GCC package ids instead of being rebuilt from source.

# pj_select_sanitizer_compiler <lane>
#   Exports CC/CXX (and PJ_SANITIZER_CLANG_VERSION) for the asan and tsan lanes;
#   does nothing for any other lane. Returns 1 when the required Clang is missing.
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
