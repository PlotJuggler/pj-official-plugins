#!/usr/bin/env bash
# Local Docker-based build for the data_stream_ros2 marketplace extension.
#
# The extension has a proxy + distro split: a distro-agnostic proxy .so plus
# one .so per supported ROS 2 distro, packaged into a single
# marketplace zip.
#
# Usage:
#   run-local.sh --distro <distro|all>     build distro artifact(s) only
#   run-local.sh --proxy                   build the proxy only
#   run-local.sh --bundle                  build everything + assemble + zip
#
#   [--plugins <path>] [--core <path>] [--core-repo <url>]
#   [--with-pj-app --pj4 <path>]
#
# Defaults:
#   --plugins      pj-official-plugins root inferred from this script's location
#   --core         unset (CPM fetches plotjuggler_core from GitHub at build time)
#   --core-repo    unset (used only when --core is not set; redirects the canonical
#                  plotjuggler_core git URL to the override inside the container)
#   --with-pj-app  also build pj4's pj_app target inside the container so the
#                  distro plugin can be smoke-tested as an installed extension.
#                  Distro mode only; requires --pj4 <path>.
#   --pj4          path to a pj4 super-repo checkout (the one with
#                  plotjuggler_core as a submodule and pj_app as a subdir).
#                  Mounted rw at /pj4 so build artifacts persist on the host.

set -eo pipefail

usage() {
  sed -n 's/^# \{0,1\}//p' "$0" | head -25
  exit 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DISTRO_DIR="${SCRIPT_DIR}/distro"
PROXY_DIR="${SCRIPT_DIR}/proxy"
DISTROS_FILE="${SCRIPT_DIR}/distros.env"

MODE=""
DISTRO=""
PLUGINS_DIR=""
CORE_DIR=""
CORE_REPO_URL=""
WITH_PJ_APP=0
PJ4_DIR=""

set_mode() {
  if [[ -n "${MODE}" && "${MODE}" != "$1" ]]; then
    echo "error: --distro/--proxy/--bundle are mutually exclusive" >&2
    exit 2
  fi
  MODE="$1"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --distro)      set_mode "distro"; DISTRO="$2"; shift 2 ;;
    --proxy)       set_mode "proxy"; shift ;;
    --bundle)      set_mode "bundle"; shift ;;
    --plugins)     PLUGINS_DIR="$2"; shift 2 ;;
    --core)        CORE_DIR="$2"; shift 2 ;;
    --core-repo)   CORE_REPO_URL="$2"; shift 2 ;;
    --with-pj-app) WITH_PJ_APP=1; shift ;;
    --pj4)         PJ4_DIR="$2"; shift 2 ;;
    -h|--help)     usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

[[ -z "${MODE}" ]] && usage

if [[ "${WITH_PJ_APP}" == "1" ]]; then
  if [[ "${MODE}" != "distro" ]]; then
    echo "error: --with-pj-app is only supported with --distro" >&2
    exit 2
  fi
  if [[ -z "${PJ4_DIR}" ]]; then
    echo "error: --with-pj-app requires --pj4 <path> (build artifacts go under <pj4>/build/)" >&2
    exit 2
  fi
  [[ -d "${PJ4_DIR}" ]] || { echo "pj4 dir not found: ${PJ4_DIR}" >&2; exit 2; }

  # If --core was not given but pj4 has a populated plotjuggler_core submodule,
  # use it. Skips the in-container `git clone` of plotjuggler_core (which fails
  # in environments where SSH/HTTPS to github gets rewritten to a corporate
  # mirror that the container has no credentials for) by reusing the same
  # checkout pj_app is built from.
  if [[ -z "${CORE_DIR}" && -d "${PJ4_DIR}/plotjuggler_core" \
        && -n "$(ls -A "${PJ4_DIR}/plotjuggler_core" 2>/dev/null)" ]]; then
    CORE_DIR="${PJ4_DIR}/plotjuggler_core"
    echo "[run-local] auto-setting --core to ${CORE_DIR} (pj4 submodule)"
  fi
fi

# Default plugins root: two levels up from this script (pj-official-plugins).
PLUGINS_DIR="${PLUGINS_DIR:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
[[ -d "${PLUGINS_DIR}" ]] || { echo "plugins dir not found: ${PLUGINS_DIR}" >&2; exit 2; }

CORE_MOUNT_ARGS=()
if [[ -n "${CORE_DIR}" ]]; then
  [[ -d "${CORE_DIR}" ]] || { echo "core dir not found: ${CORE_DIR}" >&2; exit 2; }
  CORE_MOUNT_ARGS+=(-v "${CORE_DIR}:/core:ro")
fi

PJ4_MOUNT_ARGS=()
if [[ "${WITH_PJ_APP}" == "1" ]]; then
  # rw because the build writes /pj4/build/ inside the container, and we
  # want those artifacts (notably pj_app/pj_app) to land on the host.
  PJ4_MOUNT_ARGS+=(-v "${PJ4_DIR}:/pj4")
fi

# When PLUGINS_DIR is a git submodule, /workspace/.git is a gitlink pointing
# to ../.git/modules/<name> in the superproject — unreachable inside the
# container. Expose the real gitdir at the path the gitlink expects so any
# tool that resolves git state (CMake/CPM/git describe) keeps working.
GITDIR_MOUNT_ARGS=()
if [[ -f "${PLUGINS_DIR}/.git" ]]; then
  gitlink_rel="$(sed -n 's/^gitdir: //p' "${PLUGINS_DIR}/.git")"
  gitdir_host="$(cd "${PLUGINS_DIR}" && cd "$(dirname "${gitlink_rel}")" && pwd)/$(basename "${gitlink_rel}")"
  if [[ -d "${gitdir_host}" ]]; then
    GITDIR_MOUNT_ARGS+=(-v "${gitdir_host}:/workspace/${gitlink_rel}:ro")
  else
    echo "warning: submodule gitdir not found at ${gitdir_host}" >&2
  fi
fi

# Forward the host's SSH agent socket into the container when one is set
# (e.g. webfactory/ssh-agent in the release workflow exporting
# CORE_DEPLOY_KEY). CMake/CPM inside the container then clones private
# plotjuggler_core via SSH using the same key as the rest of the plugins'
# release flow.
SSH_MOUNT_ARGS=()
if [[ -n "${SSH_AUTH_SOCK:-}" && -S "${SSH_AUTH_SOCK}" ]]; then
  SSH_MOUNT_ARGS+=(
    -v "${SSH_AUTH_SOCK}:/ssh-agent.sock"
    -e "SSH_AUTH_SOCK=/ssh-agent.sock"
  )
fi

# Share a single Conan home across every container in this invocation
# (and, when build-release.yml's actions/cache step restores it, across
# CI runs as well). Conan keys binaries by settings hash, so containers
# with the same compiler/cppstd reuse each other's gtest + nlohmann_json
# builds (humble + iron + proxy = GCC 11 / Ubuntu 22.04;
# jazzy + rolling = GCC 13 / Ubuntu 24.04) instead of rebuilding from
# source per distro.
CONAN_CACHE_HOST="${CONAN_HOME:-${HOME}/.conan2}"
mkdir -p "${CONAN_CACHE_HOST}"
CONAN_CACHE_MOUNT_ARGS=(-v "${CONAN_CACHE_HOST}:/root/.conan2")

# ─── Distro build (one distro) ──────────────────────────────────────────────
build_distro() {
  local distro="$1" base="$2"
  # Separate tag when --with-pj-app is on so the lean CI image is never
  # rebuilt against the heavier (Qt + X libs) layer.
  local tag_suffix=""
  [[ "${WITH_PJ_APP}" == "1" ]] && tag_suffix="-with-pj-app"
  local tag="pj-ros2-builder:${distro}${tag_suffix}"

  echo "[run-local] building image ${tag} (base=${base})"
  docker build \
    --build-arg "ROS_DISTRO=${distro}" \
    --build-arg "BASE_IMAGE=${base}" \
    --build-arg "WITH_PJ_APP=${WITH_PJ_APP}" \
    -t "${tag}" \
    "${DISTRO_DIR}"

  echo "[run-local] building distro for ${distro}"
  docker run --rm \
    -e "ROS_DISTRO=${distro}" \
    -e "WITH_PJ_APP=${WITH_PJ_APP}" \
    ${CORE_REPO_URL:+-e "CORE_REPO_URL=${CORE_REPO_URL}"} \
    -v "${PLUGINS_DIR}:/workspace" \
    ${CORE_MOUNT_ARGS[@]+"${CORE_MOUNT_ARGS[@]}"} \
    ${PJ4_MOUNT_ARGS[@]+"${PJ4_MOUNT_ARGS[@]}"} \
    ${GITDIR_MOUNT_ARGS[@]+"${GITDIR_MOUNT_ARGS[@]}"} \
    ${SSH_MOUNT_ARGS[@]+"${SSH_MOUNT_ARGS[@]}"} \
    "${CONAN_CACHE_MOUNT_ARGS[@]}" \
    "${tag}"
}

# ─── Proxy build (no ROS) ──────────────────────────────────────────────────
build_proxy() {
  local tag="pj-ros2-proxy-builder:ubuntu22.04"

  echo "[run-local] building image ${tag}"
  docker build -t "${tag}" "${PROXY_DIR}"

  echo "[run-local] building proxy"
  docker run --rm \
    ${CORE_REPO_URL:+-e "CORE_REPO_URL=${CORE_REPO_URL}"} \
    -v "${PLUGINS_DIR}:/workspace" \
    ${CORE_MOUNT_ARGS[@]+"${CORE_MOUNT_ARGS[@]}"} \
    ${GITDIR_MOUNT_ARGS[@]+"${GITDIR_MOUNT_ARGS[@]}"} \
    ${SSH_MOUNT_ARGS[@]+"${SSH_MOUNT_ARGS[@]}"} \
    "${CONAN_CACHE_MOUNT_ARGS[@]}" \
    "${tag}"
}

# ─── All distro builds (iterates distros.env) ───────────────────────────────
build_all_distros() {
  while IFS=: read -r distro rest; do
    [[ -z "${distro}" || "${distro}" =~ ^# ]] && continue
    build_distro "${distro}" "${rest}"
  done < "${DISTROS_FILE}"
}

# ─── Bundle assembly: dist_ros2/ tree + ros2-topic-subscriber-linux-x86_64.zip ─
assemble_bundle() {
  local stage="${PLUGINS_DIR}/dist_ros2"
  local zip_path="${PLUGINS_DIR}/ros2-topic-subscriber-linux-x86_64.zip"
  local manifest_src="${PLUGINS_DIR}/data_stream_ros2/manifest.json"
  local proxy_so="${PLUGINS_DIR}/build_ros2_proxy/Release/bin/libros2_stream_plugin.so"

  [[ -f "${manifest_src}" ]] || { echo "manifest not found: ${manifest_src}" >&2; exit 3; }
  [[ -f "${proxy_so}"     ]] || { echo "proxy .so not found: ${proxy_so}"     >&2; exit 3; }

  echo "[run-local] assembling bundle at ${stage}"
  rm -rf "${stage}" "${zip_path}"
  mkdir -p "${stage}/dist"

  cp "${proxy_so}"     "${stage}/libros2_stream_plugin.so"
  cp "${manifest_src}" "${stage}/manifest.json"

  while IFS=: read -r distro _; do
    [[ -z "${distro}" || "${distro}" =~ ^# ]] && continue
    local distro_so="${PLUGINS_DIR}/build_ros2_${distro}/Release/bin/libros2_stream_plugin-${distro}.so"
    [[ -f "${distro_so}" ]] || { echo "distro .so not found for ${distro}: ${distro_so}" >&2; exit 3; }
    mkdir -p "${stage}/dist/${distro}"
    cp "${distro_so}" "${stage}/dist/${distro}/libros2_stream_plugin-${distro}.so"
  done < "${DISTROS_FILE}"

  echo "[run-local] tree:"
  find "${stage}" -type f | sort

  echo "[run-local] creating ${zip_path}"
  ( cd "${stage}" && zip -r "${zip_path}" . )

  echo "=== Bundle OK ==="
  echo "  tree: ${stage}"
  echo "  zip:  ${zip_path}"
}

# ─── Dispatch ──────────────────────────────────────────────────────────────
case "${MODE}" in
  distro)
    if [[ "${DISTRO}" == "all" ]]; then
      build_all_distros
    else
      base="$(grep "^${DISTRO}:" "${DISTROS_FILE}" | cut -d: -f2-)"
      [[ -z "${base}" ]] && { echo "unknown distro: ${DISTRO}" >&2; exit 2; }
      build_distro "${DISTRO}" "${base}"
    fi
    ;;
  proxy)
    build_proxy
    ;;
  bundle)
    build_all_distros
    build_proxy
    assemble_bundle
    ;;
esac
