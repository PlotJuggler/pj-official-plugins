#!/usr/bin/env bash
# E2E acceptance for the Mosaico headless layout re-import (MANUAL script),
# run headless against a live Mosaico server through PJ4's
# --exit-after-layout observation channel. Three legs over ONE generated
# layout:
#
#   A. cold + UNTRUSTED: no environment allowlist, no cache entry -> the host
#      must NOT auto-import (no network fetch, no cache entry appears) and
#      must surface the needs-confirmation diagnostic.
#   B. cold + TRUSTED: MOSAICO_TRUSTED_ORIGINS allowlists the target server
#      origin and the MOSAICO_URL-guarded env key lets the provider
#      re-download the descriptor headlessly; the HOST caches the completed
#      request and commits the restore (exit 0). This is the "layout opened
#      on another machine" scenario.
#   C. warm + NO CREDENTIALS: the host-cache entry from leg B present, no api
#      key in the environment at all -> the restore commits instantly from
#      the host cache (exit 0). Zero-network by construction: without a key
#      the server would refuse.
#
# STATUS: the provider side (descriptor parse, trust allowlist, headless
# credential guard, startImport job) ships in this repo; the HOST side —
# headless descriptor-import on layout restore, the host request cache, and
# its cache directory layout — lands with PJ4 #633. Until that merges this
# script needs:
#   - PJ4_HOST_CACHE_DIR: the host's request-cache directory for the run
#     (leg A/B/C assert on the identity-addressed entry appearing there),
#   - the <materialize identity=... provider=...> layout element accepted by
#     the PJ4 restore path (shape below tracks the #633 design; adjust here
#     if the host shape drifts before freezing).
#
# Requirements: a built PJ4 tree (PJ4_DIR), the toolbox plugin build output
# in this repo's build/ dir, and MOSAICO_API_KEY in the environment (this
# script never reads stored credentials).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PJ4_DIR="${PJ4_DIR:-$HOME/ws_plotjuggler/PJ4}"
HOST_CACHE="${PJ4_HOST_CACHE_DIR:-}"
[[ -n "${HOST_CACHE}" ]] || {
  echo "FATAL: set PJ4_HOST_CACHE_DIR to the host request-cache directory (needs PJ4 #633)" >&2
  exit 64
}

SERVER_URI="${MOSAICO_E2E_SERVER:-grpc+tls://demo.mosaico.dev:6726}"
SEQUENCE="${MOSAICO_E2E_SEQUENCE:-bonirob_2016-04-20-15-43-50_10}"
TOPIC="${MOSAICO_E2E_TOPIC:-/odometry/odometry}"
START_NS="${MOSAICO_E2E_START_NS:-1461159829761465942}"
END_NS="${MOSAICO_E2E_END_NS:-1461159838697221495}"

# Strict scheme://host:port origin (for the trust allowlist) and the
# schemeless host:port the descriptor carries.
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
DESCRIPTOR_ORIGIN="${SERVER_ORIGIN#*://}"

TOOLBOX_SO="${REPO}/build/toolbox_mosaico/Release/bin/libtoolbox_mosaico_plugin.so"
[[ -f "${TOOLBOX_SO}" ]] || {
  echo "FATAL: plugin build output missing (build toolbox_mosaico first)" >&2
  exit 64
}
[[ -x "${PJ4_DIR}/run.sh" ]] || { echo "FATAL: PJ4_DIR=${PJ4_DIR} has no run.sh" >&2; exit 64; }

# The test credential comes ONLY from the environment (never printed). This
# script deliberately never reads ~/.config/PlotJuggler — that directory holds
# real credentials and is off-limits to tooling.
API_KEY="${MOSAICO_API_KEY:-}"
[[ -n "${API_KEY}" ]] || {
  echo "FATAL: set MOSAICO_API_KEY for ${SERVER_ORIGIN} (stored credentials are never read)" >&2
  exit 64
}

WORK="$(mktemp -d /tmp/mosaico-e2e-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
XDG="${WORK}/xdg"
PLUGINS="${WORK}/plugins"
mkdir -p "${XDG}" "${PLUGINS}"
ln -s "${TOOLBOX_SO}" "${PLUGINS}/"

# Generate the layout: canonical descriptor bytes (the 'mosaico.pull' v1
# shape, docs/source-descriptor-vectors.json) -> identity -> the materialize
# element PJ4 saves.
LAYOUT="${WORK}/e2e.pj4.xml"
HEX="$(python3 - "$DESCRIPTOR_ORIGIN" "$SEQUENCE" "$TOPIC" "$START_NS" "$END_NS" "$LAYOUT" <<'PYEOF'
import hashlib, json, sys
origin, seq, topic, start_ns, end_ns, layout_path = sys.argv[1:7]
canonical = json.dumps(
    {"kind": "mosaico.pull",
     "request": {"end_ns": end_ns, "origin": origin, "sequence": seq, "start_ns": start_ns, "topics": [topic]},
     "v": 1},
    separators=(",", ":"), sort_keys=True, ensure_ascii=False)
hex_digest = hashlib.sha256(canonical.encode()).hexdigest()[:32]
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
    <fileInfo display_offset_ns="0" filename="" prefix="" timeline_order="0">
      <dataset display_offset_ns="0" source_index="0" source_name="{seq}" timeline_order="0"/>
      <materialize identity="mosaico:v1:sha256/128:{hex_digest}" provider="toolbox-mosaico"><![CDATA[{canonical}]]></materialize>
    </fileInfo>
  </previouslyLoaded_Datafiles>
  <pinned_toolboxes/>
</root>
""")
print(hex_digest)
PYEOF
)"
echo "== layout: ${LAYOUT}"
echo "== identity hex: ${HEX}"

cache_entry_present() {
  # The host cache addresses entries by the identity digest; any file or
  # directory carrying it counts.
  compgen -G "${HOST_CACHE}/*${HEX}*" > /dev/null
}

run_leg() {
  # run_leg <name> <timeout_s> <extra env as KEY=VAL...>; returns the app's
  # exit code; diagnostics land in ${WORK}/<name>.json.
  local name="$1" timeout_s="$2"
  shift 2
  local diag="${WORK}/${name}.json"
  set +e
  env -u MOSAICO_API_KEY -u MOSAICO_URL -u MOSAICO_TRUSTED_ORIGINS \
    QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="${XDG}" "$@" \
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
cache_entry_present && fail "leg A fetched despite the origin being untrusted"
grep -qi "trust" "${WORK}/A.json" || fail "leg A diagnostics carry no trust refusal"
echo "== leg A OK: no cache entry, trust refusal surfaced"

# ---- Leg B: cold + trusted + origin-bound env key ------------------------
run_leg B 300 MOSAICO_TRUSTED_ORIGINS="${SERVER_ORIGIN}" \
  MOSAICO_URL="${SERVER_URI}" MOSAICO_API_KEY="${API_KEY}" \
  || fail "leg B (trusted cold re-download) exited non-zero"
cache_entry_present || fail "leg B did not populate the host cache for ${HEX}"
echo "== leg B OK: re-downloaded + host-cached"

# ---- Leg C: warm, no credentials at all ----------------------------------
run_leg C 120 || fail "leg C (warm cache, no credentials) exited non-zero"
cache_entry_present || fail "leg C consumed/removed the host cache entry"
echo "== leg C OK: restore committed from the host cache with no credentials"

echo "E2E PASS (A: untrusted refusal, B: headless re-download, C: credential-free warm load)"
