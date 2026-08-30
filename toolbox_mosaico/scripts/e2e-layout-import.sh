#!/usr/bin/env bash
# E2E acceptance for the Mosaico canonical layout re-import, run entirely
# headless against a live Mosaico server through PJ4's --exit-after-layout
# observation channel. Three legs over ONE generated layout whose cache
# artifact lives in a sandboxed MOSAICO_CACHE_DIR:
#
#   A. cold + UNTRUSTED: no environment allowlist, no artifact -> the host must NOT
#      auto-import (no network fetch, no artifact appears) and must surface
#      the needs-confirmation diagnostic.
#   B. cold + TRUSTED: MOSAICO_TRUSTED_ORIGINS allowlists the target server
#      origin and the MOSAICO_URL-guarded env key lets the provider re-download
#      the descriptor headlessly, materialize the request-addressed artifact,
#      and commit the restore (exit 0). This is the "layout opened on another
#      machine" scenario.
#   C. warm + NO CREDENTIALS: the artifact from leg B present, no api key in
#      the environment at all -> the restore commits instantly from the cache
#      (exit 0). Zero-network by construction: without a key the server would
#      refuse.
#
# Requirements: a built PJ4 tree (PJ4_DIR), the plugin build outputs in this
# repo's build/ dirs, and an api key — MOSAICO_API_KEY in the environment, or
# a stored key for the target server in ~/.config/PlotJuggler/PlotJuggler4.conf
# (read, never printed).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PJ4_DIR="${PJ4_DIR:-$HOME/ws_plotjuggler/PJ4}"

SERVER_URI="${MOSAICO_E2E_SERVER:-grpc+tls://demo.mosaico.dev:6726}"
SEQUENCE="${MOSAICO_E2E_SEQUENCE:-bonirob_2016-04-20-15-43-50_10}"
TOPIC="${MOSAICO_E2E_TOPIC:-/odometry/odometry}"
START_NS="${MOSAICO_E2E_START_NS:-1461159829761465942}"
END_NS="${MOSAICO_E2E_END_NS:-1461159838697221495}"

SERVER_ORIGIN="$(python3 - "${SERVER_URI}" <<'PYEOF'
import sys
from urllib.parse import urlsplit

uri = urlsplit(sys.argv[1])
try:
    port = uri.port
except ValueError:
    raise SystemExit(1)
if (uri.scheme.lower() not in {"grpc", "grpc+tls"} or uri.hostname is None or port is None or port == 0
        or uri.username is not None or uri.password is not None or uri.query or uri.fragment or ":" in uri.hostname):
    raise SystemExit(1)
print(f"{uri.scheme.lower()}://{uri.hostname.lower()}:{port}")
PYEOF
)" || { echo "FATAL: invalid MOSAICO_E2E_SERVER origin" >&2; exit 64; }

TOOLBOX_SO="${REPO}/build/toolbox_mosaico/Release/bin/libtoolbox_mosaico_plugin.so"
LOADER_SO="${REPO}/build/data_load_mosaico_cache/Release/bin/libmosaico_cache_source_plugin.so"
[[ -f "${TOOLBOX_SO}" && -f "${LOADER_SO}" ]] || {
  echo "FATAL: plugin build outputs missing (build toolbox_mosaico + data_load_mosaico_cache first)" >&2
  exit 64
}
[[ -x "${PJ4_DIR}/run.sh" ]] || { echo "FATAL: PJ4_DIR=${PJ4_DIR} has no run.sh" >&2; exit 64; }

# Resolve the api key without ever printing it.
API_KEY="${MOSAICO_API_KEY:-}"
if [[ -z "${API_KEY}" ]]; then
  CONF="${HOME}/.config/PlotJuggler/PlotJuggler4.conf"
  if [[ -f "${CONF}" ]]; then
    API_KEY="$(python3 - "${CONF}" "${SERVER_URI}" <<'PYEOF'
import sys
from urllib.parse import unquote


def normalize_server_key(uri):
    value = uri.strip()
    if value.startswith("grpc+tls://"):
        value = value[len("grpc+tls://"):]
    elif value.startswith("grpc://"):
        value = value[len("grpc://"):]
    if value.endswith("/"):
        value = value[:-1]
    if not value:
        return ""
    colon = value.find(":")
    if colon < 0:
        return value.lower()
    return value[:colon].lower() + value[colon:]


target = f"mosaico/server_cache/{normalize_server_key(sys.argv[2])}/api_key"
section = ""
with open(sys.argv[1], encoding="utf-8", errors="replace") as config:
    for raw_line in config:
        line = raw_line.rstrip("\r\n")
        stripped = line.strip()
        if not stripped or stripped[0] in ";#":
            continue
        if stripped.startswith("[") and stripped.endswith("]"):
            section = unquote(stripped[1:-1]).replace("\\", "/").strip("/")
            continue
        if "=" not in line:
            continue
        raw_key, value = line.split("=", 1)
        key = unquote(raw_key.strip()).replace("\\", "/").strip("/")
        full_key = f"{section}/{key}" if section else key
        if full_key == target:
            print(value)
            break
PYEOF
)"
  fi
fi
[[ -n "${API_KEY}" ]] || {
  echo "FATAL: no MOSAICO_API_KEY and no stored key for ${SERVER_ORIGIN}" >&2
  exit 64
}

WORK="$(mktemp -d /tmp/mosaico-e2e-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
CACHE="${WORK}/cache"
XDG="${WORK}/xdg"
PLUGINS="${WORK}/plugins"
mkdir -p "${CACHE}" "${XDG}" "${PLUGINS}"
ln -s "${TOOLBOX_SO}" "${PLUGINS}/"
ln -s "${LOADER_SO}" "${PLUGINS}/"

# Generate the layout: canonical descriptor bytes -> identity -> the
# fileInfo/plugin/materialize triple PJ4 saves (same shape as Save Layout).
LAYOUT="${WORK}/e2e.pj4.xml"
HEX="$(python3 - "$SERVER_URI" "$SEQUENCE" "$TOPIC" "$START_NS" "$END_NS" "$CACHE" "$LAYOUT" <<'PYEOF'
import hashlib, json, sys
server, seq, topic, start_ns, end_ns, cache, layout_path = sys.argv[1:8]
canonical = json.dumps(
    {"end_ns": end_ns, "kind": "mosaico-sequence", "sequence": seq, "server_uri": server,
     "start_ns": start_ns, "topics": [topic], "v": 1},
    separators=(",", ":"), sort_keys=True)
hex_digest = hashlib.sha256(canonical.encode()).hexdigest()[:32]
display = json.dumps(
    {"display_name": seq, "end_ns": end_ns, "kind": "mosaico-sequence", "sequence": seq,
     "server_uri": server, "start_ns": start_ns, "topics": [topic], "v": 1},
    separators=(",", ":"), sort_keys=True)
artifact = f"{cache}/{hex_digest}.pjmosaico"
config = json.dumps({"filepath": artifact})
with open(layout_path, "w") as f:
    f.write(f"""<?xml version='1.0' encoding='UTF-8'?>
<root binding="source" format="PlotJuggler" pj4_version="4">
  <tabbed_widget id="" name="main" parent="main_window">
    <Tab containers="1" id="e2e-tab" tab_name="tab1">
      <Container>
        <DockSplitter count="1" orientation="-" sizes="1.00000000">
          <DockArea id="e2e-area" name="..."><placeholder/></DockArea>
        </DockSplitter>
      </Container>
    </Tab>
    <currentTabIndex index="0"/>
  </tabbed_widget>
  <data_processors/>
  <previouslyLoaded_Datafiles>
    <fileInfo display_offset_ns="0" filename="{artifact}" prefix="" timeline_order="0">
      <dataset display_offset_ns="0" source_index="0" source_name="{seq}" timeline_order="0"/>
      <plugin ID="Mosaico Cache Loader" manifest_id="mosaico-cache-loader"><![CDATA[{config}]]></plugin>
      <materialize identity="mosaico:v1:sha256/128:{hex_digest}" provider="toolbox-mosaico"><![CDATA[{display}]]></materialize>
    </fileInfo>
  </previouslyLoaded_Datafiles>
  <pinned_toolboxes/>
</root>
""")
print(hex_digest)
PYEOF
)"
ARTIFACT="${CACHE}/${HEX}.pjmosaico"
echo "== layout: ${LAYOUT}"
echo "== identity hex: ${HEX}"

run_leg() {
  # run_leg <name> <timeout_s> <extra env as KEY=VAL...>; returns the app's
  # exit code; diagnostics land in ${WORK}/<name>.json.
  local name="$1" timeout_s="$2"
  shift 2
  local diag="${WORK}/${name}.json"
  set +e
  env -u MOSAICO_API_KEY -u MOSAICO_URL -u MOSAICO_TRUSTED_ORIGINS \
    QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="${XDG}" \
    MOSAICO_CACHE_DIR="${CACHE}" "$@" \
    "${PJ4_DIR}/run.sh" --plugin-dir "${PLUGINS}" --nosplash \
    --layout "${LAYOUT}" --exit-after-layout --exit-after-layout-timeout "${timeout_s}" \
    --dump-diagnostics "${diag}" > "${WORK}/${name}.log" 2>&1
  local code=$?
  set -e
  echo "== leg ${name}: exit=${code} (diagnostics: ${diag})"
  return ${code}
}

fail() { echo "E2E FAIL: $*" >&2; exit 1; }

# ---- Leg A: cold + untrusted --------------------------------------------
run_leg A 120 || true
[[ -f "${ARTIFACT}" ]] && fail "leg A fetched despite the origin being untrusted"
grep -qi "trust" "${WORK}/A.json" || fail "leg A diagnostics carry no trust refusal"
echo "== leg A OK: no artifact, trust refusal surfaced"

# ---- Leg B: cold + trusted + origin-bound env key ------------------------
run_leg B 300 MOSAICO_TRUSTED_ORIGINS="${SERVER_ORIGIN}" \
  MOSAICO_URL="${SERVER_URI}" MOSAICO_API_KEY="${API_KEY}" \
  || fail "leg B (trusted cold re-download) exited non-zero"
[[ -f "${ARTIFACT}" ]] || fail "leg B did not materialize ${ARTIFACT}"
echo "== leg B OK: re-downloaded + materialized $(stat -c%s "${ARTIFACT}") bytes"

# ---- Leg C: warm, no credentials at all ----------------------------------
run_leg C 120 || fail "leg C (warm cache, no credentials) exited non-zero"
[[ -f "${ARTIFACT}" ]] || fail "leg C consumed/removed the artifact"
echo "== leg C OK: restore committed from cache with no credentials"

echo "E2E PASS (A: untrusted refusal, B: headless re-download, C: credential-free warm load)"
