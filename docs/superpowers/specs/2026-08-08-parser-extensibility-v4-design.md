# Parser Extensibility v4 — Overlapping Parsers with Host-Side Per-Route Dispatch

**Date:** 2026-08-08
**Status:** v4 — supersedes the v3 architecture (registry consulted by parsers +
structured event tapes; see git history of this file for v3). Provenance: three
adversarial Codex reviews (27 findings on v2/v3), a module-mechanics consult, a
host-code stress test against PJ4 (`DataSourceRuntimeHost`, `SessionManager`), and
DSO-safety verification against plotjuggler_sdk `origin/main` through #169.
**Scope:** plotjuggler_sdk, PJ4 host runtime, pj-official-plugins (rebuild only),
pj-plugin-registry, new template repo.

## 1. The model

There are no "parsers" vs "decoders" vs "wasm extensions". There are only **parsers**,
each a **claimant over a set of message types** within an encoding, and a **host that
dispatches each topic's two routes — scalars and objects — independently** to the best
claimant.

- parser_ros is a claimant with a large set: its builtin handler table (objects +
  specialized scalars) plus a wildcard scalar claim (generic flatten).
- parser_protobuf: same shape, different subset.
- A user extension is a claimant with a set of one (or a few) types, offering one or
  both routes.
- Today's world is the degenerate case: exactly one claimant per encoding.

A CDR message of a type nobody claims → wildcard scalar flatten, exactly today's
behavior. Install a module claiming that type's object route → objects appear; the
scalar route still runs parser_ros's flatten. Neither side knows the other exists.

**parser_ros and parser_protobuf change zero source lines.** All new complexity
concentrates in the host, which already owns parser selection, dual-route
scheduling (eager scalars / lazy objects), and a second parser instance for the
object route.

## 2. Problem (unchanged)

Custom ROS2/protobuf types cannot be rendered as builtin canonical objects without
forking the official parsers. Dominant need: novel encodings (custom-compressed
clouds, bit-packed payloads) where users already have C++ decode logic.

## 3. Goals / non-goals

**Goals**

1. Zero source changes to existing parser plugins; one rebuild against the new SDK.
2. Extensions are **functional parser modules**: one C++ source built as a native
   `.so` (trusted, full speed) or a `.wasm` (one universal artifact, sandboxed);
   both drop in one folder, both loadable by the host.
3. One uniform, host-owned selection policy; no special-cased "builtin wins" code.
4. Schema access at bind time is the backward-compatibility mechanism (§8.3).
5. All cross-boundary contracts are versioned POD in the SDK's existing idioms.
6. Marketplace distributes modules alongside plugins, wasm as a first-class
   single-artifact kind, unrestricted (trust badged, not gated).

**Non-goals / rejected:** runtime structured mode (event tapes, projection, field
masks — see §14); Lua; declarative mapping; an intermediary loader plugin
(wasmer lives in the host per maintainer decision, PJ3 precedent); supplementary
scalars smuggled through object decode (a module that wants scalars claims the
scalar route).

## 4. The claim catalog

Host-owned. Three sources:

1. **Existing parser plugins** — encodings from their manifests, exactly as today
   (= the wildcard scalar claim). No manifest changes.
2. **Their object coverage** — discovered, not declared: the host queries the new
   route-aware classification extension (§7), which `MessageParserPluginBase`
   implements automatically from the handler table. The subset stays in sync with
   the code by construction.
3. **Module manifests** — embedded claims (§8.2): wasm read from a passive custom
   section without executing code; folder-drop native read via C getters after
   `dlopen` (the trust act); marketplace-installed native from the registry index
   (derived at submit time — no pre-install execution).

Claim key: `(encoding, normalized type name | wildcard, route flags, object_type,
schema-digest set, provider id, claim id, bounded priority)`. Type-name normalization is
**per-encoding** (ROS: `pkg/msg/Type` canonical; protobuf: full name), not one
global rule. Provenance is **never** a manifest field.

## 5. Per-route dispatch

Resolved once per topic at bind time, independently for the scalar and object route:

```
per-route pin (fail-closed)
  → exact-type claims over wildcard
  → provenance tier (host-derived: bundled > marketplace > folder-drop)
  → bounded priority within tier
  → stable (provider_id, claim_id) tie-break        [one-time ambiguity diagnostic]
```

- **Pins** are per-route and fail closed: a missing/rejected pinned provider darkens
  only its pinned route (diagnostics; scalars unaffected by an object pin failure).
  Fallback past a pin requires user action.
- **Provenance tiers** come from installation source (bundled catalog state,
  marketplace records, folder placement) — unforgeable by artifacts. An unrestricted
  module can therefore never outrank parser_ros for `sensor_msgs/PointCloud2`
  without a pin, yet the comparator has no parser-specific code.
- **Candidate probing** uses ACCEPT / DECLINE / ERROR: a candidate may DECLINE at
  bind (digest mismatch, schema inspection failed, version window); the host tries
  the next. Classification failure is distinguishable from a decline. Probes run
  with the candidate's real config, the winning probe instance is retained (never
  instantiated twice), results are cached by (provider generation, encoding,
  normalized type, schema digest, config digest), and invalidated on catalog, pin,
  or config change. Probe work off the source poll thread where possible.
- **No silent provider switching**: bindings are immutable generations (§6).

## 6. Composite binding

Per topic the host builds:

```
CompositeBinding (generation-stamped)
  ScalarRouteBinding { provider, instance, config, lease }
  ObjectRouteBinding { provider, instance | lazy factory, object_type, config, lease }
```

- Two route instances even when one provider wins both routes (eager scalar and lazy
  object execution overlap; share compiled wasm modules and DSO leases, never
  mutable instances or stores without synchronization).
- **Config envelope** (versioned, host-owned): per-route provider IDs/pins, config
  JSON keyed by stable provider ID, policy/catalog generation, selected
  artifact/version, plus the legacy single parser config for backward
  compatibility. The one-config-string flow and the parser-slot UI grow route
  identities.
- **Timestamps**: under eager-scalar policies the scalar route's normalized
  timestamp is binding-wide authoritative; the object route's is validated or
  ignored. Pure-lazy uses the transport timestamp.
- **Generations**: catalog reloads, pin edits, quarantine → new binding generation;
  previously indexed lazy object entries keep their original provider leases and
  decode as recorded. Object-type changes surface through host reclassification,
  never silent metadata drift.
- Diagnostics carry route, provider id, artifact/version, native/wasm mode, and
  quarantine state.

## 7. Route-aware classification (SDK extension)

`classifySchema` today returns only `object_type` — it cannot express "scalar
claimed / object claimed / both", exact-vs-wildcard, or decline-vs-failure. A new
**tail/extension classification surface** returns:

```
{ route_flags (scalar|object), object_type, match (exact|wildcard),
  status (claimed|declined|failed) }
```

`MessageParserPluginBase` implements it automatically from its registered handler
table — official parsers get it by **recompiling only**. Legacy rule: a parser
plugin without the extension implicitly claims wildcard scalars for its manifest
encodings (this covers parser_protobuf's legacy generic path). Delivery mechanism:
the existing `pluginExtension(id)` hook (primary — zero layout change, verified
present since 0.21); tail-append per the frozen-layout discipline is the fallback
only if per-instance state proves inexpressible through it. Either way, no existing
member moves; sentinel tests stay green.

## 8. Functional parser modules

### 8.1 Authoring

One C++ source + one `<name>.module.json`, wired by `pj_add_parser_module(name
SOURCE … MANIFEST … TARGETS native wasm)`:

```cpp
#include <pj_base/parser_module/module.hpp>   // header-only, wasi-clean

class RadarScanParser : public pj::FunctionalParser {
  pj::Status bind(const pj::BindingInfo& info) override {
    // §8.3: schema available here — digest check, or full inspection
    plan_ = pj::CdrFieldLocator(info.schemaText())
                .locate({"header.stamp", "header.frame_id", "width", "payload"});
    return plan_ ? pj::Status::ok() : pj::Status::decline("unsupported schema rev");
  }
  pj::Status parseObject(pj::PayloadView payload, pj::ObjectWriter& out) override {
    pj::CdrReader r(payload, plan_);
    auto cloud = out.pointCloud();
    cloud.setFrameId(r.string(kFrameId));
    auto pts = radar::decompress(r.span(kPayload), r.u32(kWidth));
    cloud.setData(pts);                    // owned → the one honest copy
    // or cloud.setDataFromInput(r.spanRef(kPayload))  → zero-copy splice
    return pj::Status::ok();
  }
  // optional: parseScalars(...) with a typed field sink — claims the scalar route
};
PJ_FUNCTIONAL_PARSER(RadarScanParser)
```

Native target: `.so` with hidden visibility, only `pj_module_*` C exports (u64
address-token signatures identical to wasm), manifest behind C getters. Wasm
target: wasip1 **reactor** (`_initialize` once; start sections rejected), manifest
appended as exactly one custom section by the SDK post-link embedder. Handles are
instance pointers as u64; results are byte-encoded descriptors; all wire
loads/stores via explicit little-endian helpers; the pinned `static_assert` set
guards 32/64-bit drift.

### 8.2 Claims manifest

```json
{ "module_abi": 1, "name": "radar-scan-parsers", "version": "1.0.0",
  "claims": [{
    "claim_id": "radar-scan-v1",
    "encoding": "ros2msg", "type_name": "my_msgs/msg/RadarScan",
    "routes": ["object"], "object_type": "kPointCloud",
    "schema_digests": ["sha256:…", "sha256:…"],   // optional allow-list
    "priority": 0 }] }
```

One module may carry claims across encodings (e.g. `ros2msg` and `protobuf`
variants of the same product) — the decode core is shared; only thin per-encoding
wire shims differ (~25–50 lines each, measured against our own handlers).

### 8.3 Schema access = the backward-compatibility mechanism

Every claimant receives `(type_name, schema)` at bind — same inputs parser_ros
gets. Author's robustness ladder:

- **L0** hard-coded layout — brittle; digest gating strongly recommended.
- **L1** digest allow-list — unknown revision ⇒ DECLINE (+ diagnostic); never a
  silent misparse. Multiple digests = multiple supported eras of recorded data.
- **L2** bind-time schema inspection — the module computes field positions from the
  `.msg` text / descriptor set and adapts across revisions. For **CDR this is the
  only reliable evolution mechanism** (positional format); protobuf is
  evolution-tolerant by field number regardless. The authoring kit's
  **field locator** (`CdrFieldLocator` / `ProtoFieldLocator`: schema + field paths
  → offset/field-number plan, computed once at bind) makes L2 a few lines.
- **L3 (reserved)** build-time typed-view codegen: `.msg`/`.proto` → generated
  wasi-clean accessor structs; wire-format independence at build time with zero
  runtime machinery. Deferred; the authoring path is reserved.

### 8.4 Module ABI

**Version naming, to prevent confusion during execution:** the manifest's
`module_abi` (=1) versions the module export surface as a whole; it *adopts* the
sink/result semantics of `pj.parser_functional` **v2** (the plugin-side extension).
Two different contracts, two counters — a module never declares "functional v2"
itself.

The landed `pj.parser_functional.v1` shape (#168), extended to **v2**:

- scalar route: typed key/value sink (`PJ_named_field_value_t`), unchanged;
- object route: `object_type u16 + canonical PJ.* wire bytes` (host decodes with
  the existing `deserializeBuiltinObject`), **plus** an append-only spliced sink
  (`accept_object_spliced`): canonical wire with the bulk field elided + one
  `(field, offset, len)` reference into the input payload. The host validates
  against the per-type splice-eligible table and resolves with the
  `PayloadView.anchor` (empty anchor ⇒ materialize). Anchor ownership, expected
  object-type validation, and wasm u64-token rules (validated offsets, never host
  pointers) are part of the v2 contract.
- results: `0 ACCEPT / 1 DECLINE / <0 ERROR` at create/bind; `0 / <0` per message;
  fixed error buffer with copy-out accessor; output valid until the next call on
  the instance; host consumes via a with-output transaction (no module pointer
  escapes).

### 8.5 Loaders (in `plugin_host`, shared by PJ4 and pj_proto_app)

- **Native**: `dlopen(RTLD_LOCAL|RTLD_NOW)` (Windows `LoadLibraryExW` with
  dll-load-dir search), per-handle symbol resolution, `-z,defs` modules, no
  substantive init work; **v1 never unloads a loaded native module mid-session**.
- **Wasm (wasmer, statically linked, pinned build)**: exact import allow-list, no
  stdin/fs/net, export signature validation, reject start sections/`_start`,
  compile-once + store-per-bound-instance via shared-module obtain (*gate:
  prototype against wasmer 7 before freezing; fallbacks enumerated*; verify store
  thread-affinity — route calls through per-store executors if required), memory
  base re-acquired after every guest call, compacted-span segment table with
  span-id-authorized splice refs, metering/epoch deadlines, memory/stack caps.
- **Budgets (aggregate, session)**: module count/file size (pre-compile), total
  claims, active instances, total linear memory; lazy instantiation with admission
  DECLINE. Compilation bounded and off the UI thread.
- **Quarantine separates faults from data errors**: bad payloads = per-message
  diagnostics, never strikes; traps/deadlines/contract violations strike
  `(module, claim)` — 3 strikes quarantines (destroy + recreate via full
  create/bind replay), repeat disables for the session. Established bindings never
  silently switch provider.

## 9. Authoring kit — `pj_base/parser_module/`

Header-only, wasi-clean subtree; INTERFACE target `plotjuggler_sdk::parser_module`
(include paths, zero linkage). C++17 floor; SDK-owned `Span`/`Expected`/`Status`;
no throwing std paths; fallible `allocate_blob`; realloc bump arenas. Contents:
`CdrReader`, `ProtoReader`, time normalization (`readRosTime`/`readProtoTimestamp`
→ ns), `CdrFieldLocator`/`ProtoFieldLocator`, `ObjectWriter` (canonical-wire
builders per object type), `PJ_FUNCTIONAL_PARSER` macro, manifest tooling
(`pj-wasm-embed-manifest`, `pj_add_parser_module`). Enforcement: standalone wasi-sdk
compile job in CI + pinned POD `static_assert`s (that gate is what a separate
package would have provided).

## 10. Distribution — registry & marketplace

- Registry `kind: "parser_module"` alongside plugins. **Both
  module kinds accepted, unrestricted** — trust badged in the UI (sandboxed wasm vs
  trusted native), not gated (maintainer decision).
- Wasm = one universal artifact + sha256; native = per-platform artifacts as
  plugins today.
- Entries derived from the embedded manifest by submit tooling (same validation the
  host loader applies), claims listed → **searchable by message type**.
- Compatibility gating by module ABI version (the module analog of
  `min_sdk_required`); auto-update never installs what the host can't run.
- Marketplace-installed native modules: claims come from the registry index (no
  pre-install execution). Install writes into the `parser_modules/` folder; the
  host rescans on install (no restart).
- Force-retagging released modules invalidates checksums — same rule as plugins.

## 11. Error handling summary

| Failure | Behavior |
|---|---|
| Claim invalid (unknown object_type/encoding) | Rejected at catalog admission, diagnostic |
| Pinned provider missing/declines | That route fails closed; other route unaffected |
| Probe DECLINE (digest, schema inspection, version) | Next candidate; one summary diagnostic |
| Probe ERROR / classification failure | Diagnosed distinctly; next candidate |
| Per-message decode error | Object omitted / scalar row skipped; rate-limited diagnostic; no strikes |
| Trap / deadline / contract violation (wasm) | Strike → quarantine → recreate; 3× disables for session |
| Module fails load (imports/signatures/start section/manifest) | Rejected at scan, diagnostic |
| Old host (no v4 SDK) | Modules not loaded; parsers behave exactly as today |

## 12. Testing strategy

- **SDK**: claim catalog + route resolver (ordering, tiers, wildcard specificity,
  per-route pins fail-closed, decline-vs-failure, probe caching/invalidation,
  generations); route-aware classification auto-implementation; functional v2 +
  splice validation; canonical-wire writer golden tests vs
  `deserializeBuiltinObject`; authoring-kit round-trips; wasi compile gate; module
  loader tests incl. adversarial wasm fixtures (trap, deadline loop, memory bomb,
  start-section module, duplicate manifest sections, disallowed imports,
  admission limits, quarantine replay); post-link ABI audit.
- **PJ4**: composite-binding refactor tests (split providers, timestamp authority,
  generations across catalog reload/pin edits, lazy entries under old
  generations), config-envelope round-trip incl. legacy configs, UI route
  identities.
- **plugins repo**: rebuild-only verification for every parser (zero source diff,
  sentinel tests green), E2E: MCAP with custom type + example module (both
  targets) → screenshot-verified cloud; scalar-route regression suite proving
  envelope series unchanged.
- **Benchmarks**: probe cost at 10/100/1000 topics × N claimants; native vs wasm
  decode overhead on a 1MB cloud fixture; regression budgets from baselines.

## 13. Milestones & PR map (minimized)

| PR | Repo | Contents |
|---|---|---|
| 1 | plotjuggler_sdk | Route-aware classification ext · functional v2 + splice · module ABI header · claim catalog + route resolver (host lib; harvests the v3 M1 snapshot's copy/lease/diagnostic patterns) · native + wasm loaders in `plugin_host` (wasmer statically linked) · authoring kit + tools + wasi gate → **SDK 0.22 release** |
| 2 | PJ4 | Composite-binding refactor (`DataSourceRuntimeHost`), config envelope, per-route pins + parser-slot UI extension, diagnostics attribution, binding generations, module folder scan at startup + rescan-on-install |
| 3 | pj-official-plugins | SDK bump: rebuild-only for all parsers (zero source changes verified) + E2E fixtures/example module + benchmarks |
| 4 | pj-plugin-registry (+ PJ4 marketplace bits) | `kind: parser_module` schema, submit tooling, marketplace wasm artifact support |
| 5 | new repo + docs | Template repo (one source, two build presets), authoring guide |

Gates: PR 2/3 require PR 1's release. The wasmer shared-module/thread-affinity
prototype is a PR 1 exit criterion. M1 v3-snapshot harvest list is recorded in the
stress-test report (keep: copies/snapshots/leases/tie-diagnostics/test structure;
prune: provider family, parser-facing service, tapes).

## 14. Deferred / rejected

**Rejected:** runtime structured mode (event/binding tapes, projection, field
masks) — cross-encoding schemas are rarely field-aligned, CDR name-robustness was
illusory, and the machinery drew the majority of adversarial findings; Lua;
declarative mapping; loader-intermediary plugin; manifest-by-execution for wasm;
priority as a trust mechanism.
**Deferred:** typed-view codegen (L3, reserved authoring path); per-thread wasm
instance pools beyond store-per-instance; module options UI; vendored
single-header authoring kit; transactional scalar rollback.

## 15. Design history — options considered and why they lost

The path to v4, kept for institutional memory (each non-adopted shape in a line or
two; full detail in this file's git history and the review transcripts).

**Initial option survey.**
- *Declarative mapping files* ("field X is the pointcloud") — solved the
  embedded-standard-type case; the real need is novel encodings requiring code.
- *Lua scripting* — interpreted per-point loops can't do codec work; a second,
  permanently weaker extension ABI.
- *Out-of-band conversion / schema hints* — documentation, not architecture.
- *Decode-downstream* (wrap bytes, decode at render — the
  `kCompressedPointCloud`/`kVideoFrame` pattern) — already exists; orthogonal;
  per-encoding not per-schema.
- *Native codec plugins + WASM modules* — survived, evolved into v4's modules.

**v1–v3: ObjectDecoder registry consulted by parsers + dual-mode ABI.** Parsers
grew a fallback rung querying a registry service; decoders got raw bytes or a
host-emitted **structured event tape** (SAX-for-CDR/protobuf with bind-time
projection). Three adversarial reviews (27 findings) kept finding the same class of
problem: the tape/projection/mask machinery was the bulk of the contract surface
and of the defects. Two external discoveries reshaped it: PJ3's working wasm-parser
prototype (validated wasmer packaging + guest feasibility; taught ABI-drift,
memory-reacquisition, and reactor-model lessons), and SDK #166/#168 landing a
canonical `PJ.*` wire codec + functional parser protocol — replacing our invented
output format with reuse.

**Harmonization step.** "Decoder = one C++ source → `.so` or `.wasm`" replaced the
two-shaped authoring story; the authoring kit moved into `pj_base/decoder/` (no new
package; wasi CI gate carries the constraint); wasmer moved from an intermediary
`parser_extensions` plugin into the host (PJ3 precedent), dissolving the provider
plugin family and its lease machinery.

**The collapse into v4.** Interrogating "how is a decoder technically different
from a parser?" showed it isn't: a decoder is a parser restricted to one type and
(optionally) one route, and the v4 parser contract already splits the routes.
Host-side per-route dispatch made the parsers' fallback branches, the
parser-facing registry service, and the binding-context service unnecessary —
existing parsers now change zero source lines. A host-code stress test then
hardened it: provenance-tier trust (priority alone was a security regression),
composite bindings with immutable generations, timestamp authority, and the
functional-v2 splice gap.

**Durable learnings.**
1. *Schema divergence beats wire abstraction*: even Foxglove's own dual-encoding
   schemas aren't field-aligned — the event tape abstracted the wrong layer. The
   true cross-encoding convergence point is the **canonical object** (output),
   not the wire (input).
2. *Reuse over invention*: every place we replaced an invented contract with a
   landed one (#166/#168 wire codec, ServiceRegistry idiom, functional protocol)
   deleted findings wholesale.
3. *Trust must be host-derived* — never a field an artifact writes.
4. *Schema access at bind is the evolution mechanism* (CDR is positional; digest
   gating turns drift into DECLINE, not garbage).
5. *Measure before architecting*: the feared per-encoding boilerplate is 25–50
   lines with good readers — measured in our own handlers — which is what made
   dropping the tape machinery safe.
6. *The zero-source-change constraint produced the best architecture*: every
   design that required parsers to participate was worse than the one that
   didn't.

## 16. Decisions log

1. **Overlapping type-subset claimants; per-route host dispatch; zero parser source
   changes** (maintainer, 2026-08-08).
2. **Wasmer in the host** (`plugin_host`), PJ3 precedent — no intermediary plugin
   (maintainer).
3. **Modules = functional parsers**: one concept; "decoder" vs "wasm parser"
   differs only in claims (maintainer).
4. **Structured mode dropped; readers + bind-time field locators + reserved
   codegen** replace it (analysis + stress test).
5. **Trust is host-derived provenance, never manifest data**; uniform comparator;
   per-route fail-closed pins (stress-test blocker resolution).
6. **Schema access at bind = compatibility mechanism** (L0–L3 ladder; DECLINE
   semantics) (maintainer insight).
7. **Output = canonical PJ.* wire + splice** via functional v2 — reuse of #166/#168.
8. **Marketplace unrestricted, both kinds; wasm first-class universal artifact**
   (maintainer).
9. **Authoring kit inside pj_base** (`pj_base/parser_module/`), wasi CI gate as the
   enforcement (maintainer).
10. **Immutable binding generations; scalar-route timestamp authority** (stress-test
    resolution).
