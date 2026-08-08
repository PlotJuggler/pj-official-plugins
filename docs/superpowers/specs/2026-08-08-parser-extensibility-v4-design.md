# Parser Extensibility v4 — Overlapping Parsers with Host-Side Per-Route Dispatch

**Date:** 2026-08-08
**Status:** v4 — supersedes the v3 architecture (registry consulted by parsers +
structured event tapes; see git history of this file for v3). Provenance: four
adversarial Codex reviews (27 findings on v2/v3 + a final specification-closure
review whose 8 findings froze the normative ABI surfaces in §7/§8.4), a
module-mechanics consult, a host-code stress test against PJ4
(`DataSourceRuntimeHost`, `SessionManager`), and DSO-safety verification against
plotjuggler_sdk `origin/main` through #169.
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
   platform shared library (`.so`/`.dylib`/`.dll`; trusted, full speed) or a
   `.wasm` (one universal artifact, sandboxed); both drop in one folder, both
   loadable by the host.
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
   (= the universal wildcard scalar claim, §7 — held by every parser plugin
   regardless of extension presence). No manifest changes.
2. **Their exact-type coverage** — discovered, not declared: the host queries the
   route-aware classification extension (§7), which `MessageParserPluginBase`
   implements automatically from the handler table. The subset stays in sync with
   the code by construction; it adds to the wildcard, never replaces it.
3. **Module manifests** — embedded claims (§8.2): wasm read from a passive custom
   section without executing code; folder-drop native read via C getters after
   `dlopen` (the trust act); marketplace-installed native from the registry index
   (derived at submit time — no pre-install execution).

Claim key: `(encoding, normalized type name | wildcard, route flags, object_type,
schema-digest set, provider id, claim id, bounded priority)`. Identity and
normalization rules (normative):

- **provider id** = the plugin manifest's `"id"` for parser plugins, the module
  manifest's `"id"` for modules (§8.2) — globally stable, reverse-DNS
  recommended; `name` is display-only everywhere. Pins, config keys, receipts,
  ties, and upgrades all key on it.
- **claim id** is unique within its provider; the catalog key is
  `(provider_id, claim_id)`, and a duplicate rejects the whole module at
  admission.
- **priority** is int32 in `[-1000, 1000]`; out-of-range claims are rejected at
  admission (explicit, never clamped).
- **wildcard** is spelled `"type_name": "*"`. Modules may declare it for the
  scalar route; a wildcard **object** claim is rejected at admission (the object
  route requires an exact type and an `object_type`). `object_type` is required
  iff `routes` contains `"object"`, forbidden otherwise.
- **schema digest** = SHA-256 over the exact schema byte sequence the host
  delivers to `bind_schema`/`bind` — no normalization; the digest set names
  supported recorded eras of those bytes.
- **encoding names** come from the SDK-owned registry (the same strings parser
  manifests use: `ros2msg`, `protobuf`, …); matching is case-sensitive; a claim
  with an unknown encoding is admitted but never matched (diagnostic).
- Type-name normalization is **per-encoding** (ROS: `pkg/msg/Type` canonical;
  protobuf: full name), owned by the host catalog, not by claimants.
- Provenance is **never** a manifest field.

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
  or config change.
- **Probe threading (normative):** the parser ABI marks create/destroy/bind/
  bind-schema/config as main-thread-class operations, and classification is only
  meaningful after them — so the host marshals the whole probe sequence to one
  designated **parser-control executor** (the thread class those slots already
  require), never to arbitrary workers or the source poll thread:
  `create → bind services → bind schema → load provider config → route
  classification/probe → retain winner`. Only functional parse calls run on
  their declared stream/worker thread class.
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
- **Timestamps (one rule)**: for a given message, the binding-wide timestamp is
  the scalar route's normalized timestamp **when a successful scalar result
  exists for that message**; otherwise (scalar route absent, declined, or failed
  on that message) it is the transport timestamp, with a rate-limited scalar
  diagnostic. Object-route timestamps are never authoritative; a mismatch beyond
  tolerance is a rate-limited diagnostic, not an error. The object route stays
  independently eligible when the scalar route fails.
- **Generations pin descriptors, not instances**: a generation immutably records
  per-route `(provider id, artifact version + checksum, config digest)`.
  Catalog reloads, pin edits, and claim changes create a **new** generation;
  quarantine recreation (§8.5) replays create/bind for the **same** descriptor
  and therefore stays within its generation. Previously indexed lazy object
  entries decode as recorded via their generation's descriptor; if that
  descriptor becomes unsatisfiable — the claim was disabled for the session, or
  the artifact was upgraded/removed — those entries **fail closed** with a
  diagnostic. No fallback provider is ever substituted. Object-type changes
  surface through host reclassification, never silent metadata drift.
- Diagnostics carry route, provider id, artifact/version, native/wasm mode, and
  quarantine state.

## 7. Route-aware classification (SDK extension)

`classifySchema` today returns only `object_type` — it cannot express "scalar
claimed / object claimed / both", exact-vs-wildcard, or decline-vs-failure.

**Delivery is the existing `get_plugin_extension(id)` hook — committed, no
fallback.** The hook already receives the instance `ctx`, so per-instance state is
expressible; the tail-append alternative is dropped. No vtable member moves;
sentinel tests stay green. Normative surface (frozen in PR 1a):

- Extension ID: `PJ_PARSER_ROUTE_CLAIMS_EXTENSION_V1 = "pj.parser_route_claims.v1"`.
- Table (append-only, `struct_size`-gated, every slot `PJ_NOEXCEPT`):

```c
typedef struct PJ_route_classification_v1_t {
  uint16_t route_flags;   /* bit0 scalar, bit1 object */
  uint16_t match;         /* 0 exact, 1 wildcard */
  uint16_t status;        /* 0 claimed, 1 declined, 2 failed */
  uint16_t object_type;   /* PJ_BUILTIN_OBJECT_TYPE_*; NONE unless object claimed */
} PJ_route_classification_v1_t;

typedef struct PJ_parser_route_claims_v1_t {
  uint32_t struct_size;   /* sizeof(this table revision) */
  /* [thread-class of classify_schema: thread-safe, pure, no host side-effects]
   * Called after bind_schema() on the same instance. Returns false + out_error
   * only on classification FAILURE (status is then ignored); DECLINE is a
   * successful return with status=declined. */
  bool (*classify_routes)(void* ctx, PJ_string_view_t type_name, PJ_bytes_view_t schema,
                          PJ_route_classification_v1_t* out, PJ_error_t* out_error) PJ_NOEXCEPT;
} PJ_parser_route_claims_v1_t;
```

`MessageParserPluginBase` implements it automatically from its registered handler
table — official parsers get it by **recompiling only**. A recompiled plugin
exposes both the legacy `classify_schema` tail slot and this extension unchanged;
hosts prefer the extension when present and fall back to `classify_schema` (object
route only, scalar assumed wildcard) when absent or on failure.

**Universal wildcard rule (resolves the legacy/protobuf contradiction):** every
parser plugin receives the manifest-derived **wildcard scalar claim** for its
declared encodings — with or without the extension, which only *adds* exact
handler-table claims and never removes the wildcard. parser_protobuf's generic
descriptor-driven flatten is exactly that wildcard claim; parser_ros's generic
flatten likewise. A plugin that genuinely cannot scalar-flatten a schema signals
it per-topic via DECLINE at probe time, not by claim absence.

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

Native target: platform shared library with hidden visibility, only `pj_module_*`
C exports (u64 address-token signatures identical to wasm), manifest behind C
getters. Wasm target: wasip1 **reactor** (`_initialize` once; start sections
rejected), manifest appended as exactly one custom section by the SDK post-link
embedder. The complete export set, token semantics, and byte formats are the
normative §8.4.2/§8.4.3; the pinned `static_assert` set guards 32/64-bit drift.

### 8.2 Claims manifest

```json
{ "module_abi": 1,
  "id": "com.example.radar-scan-parsers",   // globally stable provider id (§4)
  "name": "Radar Scan Parsers",             // display-only
  "version": "1.0.0",
  "claims": [{
    "claim_id": "radar-scan-v1",            // unique within the module
    "encoding": "ros2msg", "type_name": "my_msgs/msg/RadarScan",
    "routes": ["object"], "object_type": "kPointCloud",
    "schema_digests": ["sha256:…", "sha256:…"],   // optional allow-list
    "priority": 0 }] }
```

`id` mirrors the plugin manifest's required `"id"` key and obeys the §4 identity
rules (pins, config, receipts, upgrades all key on it; `name` never does).

One module may carry claims across encodings (e.g. `ros2msg` and `protobuf`
variants of the same product) — the decode core is shared; only thin per-encoding
wire shims differ (~25–50 lines each, measured against our own handlers).

### 8.3 Schema access = the backward-compatibility mechanism

Every claimant receives `(type_name, schema)` at bind — same inputs parser_ros
gets. Author's robustness ladder:

- **L0** hard-coded layout — brittle; digest gating strongly recommended.
- **L1** digest allow-list — unknown revision ⇒ DECLINE (+ diagnostic); never a
  silent misparse. Multiple digests = multiple supported eras of recorded data.
- **L2** bind-time schema inspection — the module derives field access from the
  `.msg` text / descriptor set and adapts across revisions. For **CDR this is the
  only reliable evolution mechanism** (positional format); protobuf is
  evolution-tolerant by field number regardless. The authoring kit's
  **field locator** (`CdrFieldLocator` / `ProtoFieldLocator`) compiles schema +
  field paths **once at bind into a runtime traversal plan** — for CDR, any
  variable-length field shifts everything after it, so payload-independent
  offsets don't generally exist; the plan is a per-message walk (with position
  caching across field reads), never a static offset table. Protobuf plans
  resolve to field-number paths. Contract details in §9. This makes L2 a few
  lines for the author.
- **L3 (reserved)** build-time typed-view codegen: `.msg`/`.proto` → generated
  wasi-clean accessor structs; wire-format independence at build time with zero
  runtime machinery. Deferred; the authoring path is reserved.

### 8.4 Frozen contracts (normative — this is the PR 1a freeze set)

**Version naming, to prevent confusion during execution:** the manifest's
`module_abi` (=1) versions the module export surface as a whole; it *adopts* the
sink/result semantics of `pj.parser_functional` **v2** (the plugin-side extension).
Two different contracts, two counters — a module never declares "functional v2"
itself.

#### 8.4.1 `pj.parser_functional.v2` (plugin extension)

ID: `PJ_PARSER_FUNCTIONAL_EXTENSION_V2 = "pj.parser_functional.v2"`. Same idioms
as the landed v1 (#168): `struct_size`-gated append-only PODs, borrowed
call-duration views, `PJ_NOEXCEPT` everywhere, [stream-thread] calls.

- **Scalar route:** unchanged from v1 (`PJ_parser_scalar_sink_v1_t`,
  `PJ_named_field_value_t`).
- **Object sink v2** extends the v1 sink append-only:

```c
typedef bool (*PJ_parser_accept_object_spliced_fn_t)(
    void* ctx, bool has_timestamp, int64_t timestamp_ns, uint16_t object_type,
    PJ_bytes_view_t partial_wire,       /* canonical PJ.* wire, bulk field elided */
    uint32_t splice_field_number,       /* PJ.* field number of the elided field  */
    uint64_t input_offset, uint64_t input_length,   /* into the parse payload     */
    PJ_error_t* out_error) PJ_NOEXCEPT;

typedef struct PJ_parser_object_sink_v2_t {
  uint32_t struct_size;
  void* ctx;
  PJ_parser_accept_object_fn_t accept_object;               /* v1 slot, same type */
  PJ_parser_accept_object_spliced_fn_t accept_object_spliced;
} PJ_parser_object_sink_v2_t;

typedef struct PJ_parser_functional_v2_t {
  uint32_t struct_size;
  PJ_parser_parse_scalars_fn_t parse_scalars;               /* v1 type, unchanged */
  bool (*parse_object)(void* plugin_ctx, int64_t timestamp_ns, PJ_payload_t payload,
                       const PJ_parser_object_sink_v2_t* sink, PJ_error_t* out_error) PJ_NOEXCEPT;
} PJ_parser_functional_v2_t;
```

- **Splice rules:** at most **one** splice reference per object (a second
  spliced call for the same message is a contract violation).
  `splice_field_number` must appear in the per-type splice-eligible table frozen
  in `builtin_object_abi.h`; `input_offset/input_length` index the exact payload
  bytes passed to `parse_object` (input-space; host bounds-validates). The host
  resolves the reference with `PJ_payload_t.anchor` (empty anchor ⇒ the host
  materializes a copy). Expected-object-type validation and anchor ownership are
  exactly v1's (#168) rules.
- **Compatibility:** a rebuilt plugin exposes **both** IDs. New host + old
  plugin: host queries v2, falls back to v1 (no splice). Old host + new plugin:
  host only queries v1; the base class materializes bulk fields into full
  canonical wire. Both directions work with no negotiation beyond
  `get_plugin_extension`.
- **Outcome taxonomy:** `0 ACCEPT / 1 DECLINE / <0 ERROR` exists only at
  create/bind. Per message there is no decline: `true` = success, `false` +
  `PJ_error_t` = failure, with `extended_kind` distinguishing **parser data
  error** (bad payload — diagnostic, no strike), **sink rejection** (host-side;
  propagated as call failure with the sink's error), and **contract violation**
  (double splice, ineligible field, bounds — strikes, §8.5).

#### 8.4.2 Module export ABI (`module_abi = 1`)

One C export set, identical for both targets. Every address-like value is a
`uint64_t` **module-space token**: for native, a process pointer (host buffers
may be passed directly — zero copy); for wasm, a linear-memory offset (host
copies in via `pj_module_alloc`, re-acquires the memory base after every guest
call, and never passes host pointers).

```c
uint32_t pj_module_abi(void);                                   /* = 1          */
uint64_t pj_module_create(uint32_t claim_index);                /* 0 = failure  */
void     pj_module_destroy(uint64_t inst);
int32_t  pj_module_bind(uint64_t inst, uint64_t info_addr, uint64_t info_len);
                                          /* 0 ACCEPT / 1 DECLINE / <0 ERROR    */
int32_t  pj_module_parse(uint64_t inst, uint64_t in_addr, uint64_t in_len,
                         uint64_t out_addr_ptr, uint64_t out_len_ptr);  /* 0/<0 */
uint64_t pj_module_last_error(uint64_t inst, uint64_t buf_addr, uint64_t buf_cap);
uint64_t pj_module_alloc(uint64_t size);                        /* 0 = failure  */
void     pj_module_free(uint64_t addr, uint64_t size);
```

- **Lifecycle:** `create(claim_index)` → `bind` (BindingInfo bytes, §8.4.3) →
  `parse` per message → `destroy`. `claim_index` is the claim's position in the
  manifest's `claims` array; out-of-range → create fails.
- **Buffers:** input buffers for wasm are guest-allocated by the host via
  `pj_module_alloc` and freed via `pj_module_free` after the call. Output
  descriptors are module-owned, returned through `(*out_addr, *out_len)`, and
  valid until the next call on the same instance or `destroy` — the host
  consumes them inside a with-output transaction; no module pointer escapes.
- **Instance tokens** are opaque; the module validates them against its live
  table and returns `PJ_MODULE_ERR_BAD_TOKEN` for stale/unknown values, which
  the host treats as a contract violation (strike). Error codes: `0` OK, `1`
  DECLINE (bind only); `<0` reserved set (`-1` generic, `-2` bad token, `-3`
  malformed input, `-4` bad claim index, `-5` allocation failure; new codes
  append).
- **Errors:** on any negative return the module records a UTF-8 message in a
  fixed 512-byte internal buffer (NUL-truncated); the host copies it out with
  `pj_module_last_error` (returns bytes written).
- **Manifest delivery:** native exports `const char* pj_module_manifest_json(void)`
  + `uint64_t pj_module_manifest_len(void)` (pointer valid for module lifetime);
  wasm carries the manifest **only** in a custom section named
  `pj_parser_module_manifest` holding the exact UTF-8 JSON bytes — exactly one;
  zero or duplicates reject the artifact at scan. Wasm modules are wasip1
  **reactors**: `_initialize` required, `_start` or a start section rejected.

#### 8.4.3 Byte formats (all integers little-endian)

- **BindingInfo (host → module, `pj_module_bind`):** header
  `u16 version=1 · u16 route (1 scalar / 2 object) · u32 claim_index ·
  u16 expected_object_type · u16 reserved · u32 field_count`, then
  `field_count` × `(u32 offset, u32 len)` into the block, fixed order:
  encoding, type_name, schema bytes, claim_id, config JSON, schema digest
  (`sha256:…` ASCII or empty). Readers accept and ignore extra trailing fields
  (append-only). Config JSON here is the module's per-provider entry from the
  host's config envelope (§6); there is no other config channel.
- **Parse input (host → module):** `u8 flags (bit0 has_timestamp) · 7×u8 pad ·
  i64 timestamp_ns · u64 payload_len · payload bytes`. Splice offsets in the
  output are relative to the **payload region** (byte 0 = first payload byte);
  the host maps them back to its original message buffer — the single
  input-space offset rule.
- **Output descriptor (module → host):** header `u16 version=1 · u8 route ·
  u8 reserved`, then per route — object: `u16 object_type · u16 splice_count
  (0|1) · u32 splice_field_number · u64 splice_offset · u64 splice_len ·
  u64 wire_len · wire bytes` (canonical PJ.* wire; host decodes with the landed
  `deserializeBuiltinObject`, then applies the same splice validation as
  §8.4.1); scalar: `u8 has_timestamp · 7×u8 pad · i64 timestamp_ns ·
  u32 field_count`, then per field `u32 name_off · u32 name_len · u8 value_kind
  · value bytes`, with `value_kind` mirroring `PJ_named_field_value_t`
  (0 f64, 1 i64, 2 u64, 3 bool, 4 string as u32 len + UTF-8 bytes).
- The pinned `static_assert` set plus golden byte fixtures (§12) freeze these
  layouts; the PR 1a static wasm conformance slice (§13) verifies them against
  a real wasi-sdk-built reactor before 0.22 ships.

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
  create/bind replay), repeat disables for the session. Same-descriptor
  recreation stays within its binding generation; disablement makes outstanding
  lazy entries fail closed (§6). Established bindings never silently switch
  provider.

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

**Reader/locator contract (normative):**

- **Accepted schema inputs:** ROS 2 concatenated `.msg` bundles as delivered on
  `ros2msg` channels (MCAP/ROS 2 convention); protobuf serialized
  `FileDescriptorSet`. Anything else → bind-time error `Status`.
- **CDR:** XCDR1, endianness from the encapsulation header, alignment relative
  to the encapsulation start; nested types, fixed arrays, sequences, and strings
  traversed per the compiled plan. Every read is bounds-checked against the
  payload; malformed or truncated input yields an error `Status`, **never** UB.
  Sequence/string lengths are validated against remaining bytes before
  allocation; traversal depth is capped (64). Random-access field reads reuse
  cached traversal positions (monotone re-reads never re-walk from byte 0).
- **Protobuf:** unknown fields skipped, packed and unpacked repeated encodings
  both accepted, duplicate scalar occurrences last-wins, recursion depth capped
  (64), truncated varints/groups → error `Status`.
- **Time normalization:** `readRosTime`/`readProtoTimestamp` reject
  out-of-range values (would overflow int64 ns) with an error `Status` rather
  than saturating silently.

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
- **Install receipts (provenance anchor):** marketplace and folder-drop
  artifacts share `parser_modules/`, so folder location cannot carry provenance.
  On install the host writes a receipt into its own catalog state —
  `(module id, version, sha256, provenance tier, install time, source)`. At scan,
  an artifact whose checksum matches a receipt gets that receipt's tier;
  anything else is folder-drop. Receipts are host state, never artifact content.
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
  `deserializeBuiltinObject`; authoring-kit round-trips; wasi compile gate;
  **static wasm ABI conformance (1a):** wasi-sdk reactor fixture with export/
  signature audit, manifest-section embed/read-back, §8.4.3 golden byte
  fixtures — no execution; module loader tests incl. adversarial wasm fixtures
  (1b: trap, deadline loop, memory bomb, start-section module, duplicate
  manifest sections, disallowed imports, admission limits, quarantine replay);
  post-link ABI audit.
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

## 13. Milestones & PR map

| PR | Repo | Contents |
|---|---|---|
| 1a | plotjuggler_sdk | **SDK core, wasmer-free**: route-aware classification ext · functional v2 + splice · **complete dual-target module ABI header (frozen here)** · claim catalog + route resolver · native module loader · authoring kit core (readers, locators, ObjectWriter, native macro target) + wasi-clean CI gate · **static wasm ABI conformance slice** (one wasi-sdk-built reactor fixture; static export/signature audit; manifest custom-section embed + read-back; §8.4.3 descriptor-byte and token-width assertions — no wasmer, no execution) → **release 0.22** |
| 1b | plotjuggler_sdk | **SDK wasm**: wasmer loader (statically linked, pinned; hardening, budgets, quarantine) · wasm macro target + `pj-wasm-embed-manifest` · adversarial wasm fixtures → **release 0.23**. Exit criterion: wasmer-7 shared-module-obtain + store thread-affinity prototype |
| 2 | PJ4 | Composite-binding refactor (`DataSourceRuntimeHost`), config envelope, per-route pins + parser-slot UI extension, diagnostics attribution, binding generations, module folder scan at startup + rescan-on-install, marketplace `parser_module` support (wasm artifacts activate with 0.23) |
| 3 | pj-official-plugins | SDK bump: rebuild-only for all parsers (zero source changes verified) + native-module E2E fixtures + benchmarks + authoring guide; wasm E2E addendum after 0.23 |
| 4 | pj-plugin-registry | `kind: parser_module` schema + submit tooling + validation. The schema covers both artifact kinds from day one; **wasm artifact admission stays dormant until 0.23 ships** (mirrors PR 2's marketplace gate) |
| — | new repo | Template repo bootstrap (one source, two build presets) — repo creation, not a PR |

Gates: PRs 2/3/4 need only **0.22** (the ABI is complete there); everything
wasm-*execution*-facing (1b, wasm E2E, marketplace/registry wasm-artifact
admission) activates with **0.23** without touching any contract — 1b is a
second loader for an already-frozen ABI, so a wasmer setback delays wasm, never
the architecture. The 1a conformance slice is what makes that freeze safe: the
export surface, manifest section, and descriptor bytes are proven against a real
wasm artifact before 0.22 ships, leaving 1b zero room to discover ABI defects.
Rationale for the 1a/1b split: wasmer packaging and the shared-module prototype
are the highest-risk items and review as runtime/security work, while 1a reviews
as ABI/contract work.

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
11. **Classification delivery committed to `get_plugin_extension`** (no
    tail-append fallback) **+ universal wildcard scalar claim** for every parser
    plugin regardless of extension presence (final-review blocker resolution).
12. **Generations pin descriptors, not instances**: quarantine recreation stays
    in-generation; disablement/upgrade fails outstanding lazy entries closed —
    never a substituted provider (final-review resolution).
13. **Stable module `id` in the manifest** (name display-only) + host install
    receipts as the provenance anchor (final-review blocker resolution).
14. **PR 1a carries a static wasm ABI conformance slice** (wasi-sdk fixture,
    export/section/byte audits, no wasmer) so the 0.22 freeze is proven against
    a real wasm artifact before 1b (final-review resolution).
