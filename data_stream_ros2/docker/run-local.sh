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
#   [--plugins <path>] [--core <path>]
#
# Defaults:
#   --plugins  pj-official-plugins root inferred from this script's location
#   --core     unset (CPM fetches plotjuggler_core from GitHub at build time)

set -eo pipefail

usage() {
  sed -n 's/^# \{0,1\}//p' "$0" | head -16
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

set_mode() {
  if [[ -n "${MODE}" && "${MODE}" != "$1" ]]; then
    echo "error: --distro/--proxy/--bundle are mutually exclusive" >&2
    exit 2
  fi
  MODE="$1"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --distro)  set_mode "distro"; DISTRO="$2"; shift 2 ;;
    --proxy)   set_mode "proxy"; shift ;;
    --bundle)  set_mode "bundle"; shift ;;
    --plugins) PLUGINS_DIR="$2"; shift 2 ;;
    --core)    CORE_DIR="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

[[ -z "${MODE}" ]] && usage

# Default plugins root: two levels up from this script (pj-official-plugins).
PLUGINS_DIR="${PLUGINS_DIR:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
[[ -d "${PLUGINS_DIR}" ]] || { echo "plugins dir not found: ${PLUGINS_DIR}" >&2; exit 2; }

CORE_MOUNT_ARGS=()
if [[ -n "${CORE_DIR}" ]]; then
  [[ -d "${CORE_DIR}" ]] || { echo "core dir not found: ${CORE_DIR}" >&2; exit 2; }
  CORE_MOUNT_ARGS+=(-v "${CORE_DIR}:/core:ro")
fi

# ─── Distro build (one distro) ──────────────────────────────────────────────
build_distro() {
  local distro="$1" base="$2"
  local tag="pj-ros2-builder:${distro}"

  echo "[run-local] building image ${tag} (base=${base})"
  docker build \
    --build-arg "ROS_DISTRO=${distro}" \
    --build-arg "BASE_IMAGE=${base}" \
    -t "${tag}" \
    "${DISTRO_DIR}"

  echo "[run-local] building distro for ${distro}"
  docker run --rm \
    -e "ROS_DISTRO=${distro}" \
    -v "${PLUGINS_DIR}:/workspace" \
    ${CORE_MOUNT_ARGS[@]+"${CORE_MOUNT_ARGS[@]}"} \
    "${tag}"
}

# ─── Proxy build (no ROS) ──────────────────────────────────────────────────
build_proxy() {
  local tag="pj-ros2-proxy-builder:ubuntu22.04"

  echo "[run-local] building image ${tag}"
  docker build -t "${tag}" "${PROXY_DIR}"

  echo "[run-local] building proxy"
  docker run --rm \
    -v "${PLUGINS_DIR}:/workspace" \
    ${CORE_MOUNT_ARGS[@]+"${CORE_MOUNT_ARGS[@]}"} \
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
