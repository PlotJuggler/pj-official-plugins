# Parser Extensibility: Decoder Modules + Object-Decoder Registry

**Date:** 2026-08-08
**Status:** Draft v3 — harmonized decoder-module architecture; aligned to plotjuggler_sdk
`origin/main` (through #168); v2 adversarial-review findings 17–27 incorporated.
Module-packaging mechanics resolved per the mechanics consult (Codex,
2026-08-08): uniform u64 address-token ABI, post-link manifest embedder, reactor-model
wasm, with-output transaction.
**Scope:** plotjuggler_sdk (`pj_base/decoder/` subtree, registry service, provider
family), parser_ros, parser_protobuf, new plugin `parser_extensions`

## 1. Summary

Users cannot today map a custom ROS2/protobuf message type onto a builtin canonical
object (PointCloud, Image, …) without forking parser_ros/parser_protobuf. This design
adds **decoder modules**: an author writes ONE C++ source against a header-only
authoring API in the SDK, and builds it as either

- a **native module** (`.so`/`.dll`/`.dylib`) — this platform, full speed, trusted; or
- a **WASM module** (`.wasm`, wasi-sdk) — one artifact for every platform, sandboxed;

both drop into one `decoders/` folder. One official plugin, **parser_extensions**,
loads both kinds (dlopen | wasmer), normalizes them onto one internal contract, and
registers their claims with a host-brokered **ObjectDecoder registry**. parser_ros and
parser_protobuf consult the registry when binding a schema, before falling back to
generic scalar flatten.

Decode modes per claim:

- **raw** — the module receives payload bytes (schema-less/custom envelopes, or
  wrapping an existing whole-message decoder);
- **structured** — the parser walks CDR/protobuf itself and hands the module a
  tokenized **event tape** (typed fields, blobs as spans) filtered by a bind-time field
  mask; module authors never implement wire decoding. v1 supports a declared schema
  subset with deterministic bind DECLINE outside it (§8.4).

Module **output** reuses the SDK's existing canonical contract (landed in #166/#168):
`object_type (u16) + canonical PJ.* wire bytes`, decoded host-side by
`deserializeBuiltinObject()` — no bespoke output format — plus one splice extension for
zero-copy bulk passthrough (§9).

## 2. Problem

- parser_ros: static schema-name table in `ros_parser.cpp`; parser_protobuf: if-chain
  in `bindSchema()`. Adding a type means forking a plugin.
- Dominant need (confirmed): **novel encodings** — custom compression, bit-packed
  payloads — real decode logic, usually already written in C++ by the user.
- Plugins are RTLD_LOCAL and self-contained: all cross-plugin delegation must be
  host-brokered; every boundary must follow the SDK's established DSO-safe idioms
  (§4).

## 3. Goals and non-goals

**Goals**

1. One authoring experience: the same C++ class + macro builds to `.so` or `.wasm`;
   format choice is a build/trust/performance trade-off, never an architecture change.
2. Structured mode: decoder authors never reimplement CDR/protobuf (within the v1
   subset).
3. Every cross-DSO/WASM boundary uses the SDK's existing POD idioms (`PJ_*_view_t`,
   `PJ_error_t`, fat-pointer services, `struct_size`-gated vtables, noexcept).
4. Untrusted WASM modules are bounded in **access and availability**, per-call and in
   aggregate (§11.4).
5. Delegation, conflicts, and failures surface on the parser-diagnostics channel
   (`pj.parser_runtime.v1`).

**Non-goals (deferred/rejected, §16):** Lua; declarative mapping; host-owned WASM
runtime; supplementary scalars from object decode; native provider plugins as a
documented extension path (the module format is the one story; the provider family
stays available for a future advanced path).

## 4. DSO-safety baseline (verified against origin/main, 2026-08-08)

The design conforms to, and reuses, the following landed mechanisms:

| Mechanism (main) | Used here for |
|---|---|
| `PJ_service_registry_t` — ABI-FROZEN fat pointer; ABI-APPENDABLE vtable with `struct_size` | Delivery of the registry + binding context to parsers via the `ServiceRegistry` already passed to `MessageParserPluginBase::bind()`; **zero base-class layout change** (sentinel test `message_parser_abi_layout_sentinels_test.cpp` stays green by construction) |
| `service_traits.hpp` pattern (`kName`, `kMinVersion`, `View`) — as used by `pj.parser_runtime.v1`, `pj.source_promotion.v1` | `pj.object_decoder_registry.v1` and `pj.parser_binding_context.v1` are defined the same way |
| `PJ_string_view_t`, `PJ_bytes_view_t`, `PJ_error_t`, noexcept trampolines | All new C surfaces use these types verbatim — no parallel inventions |
| `pj.parser_functional.v1` object sink: `object_type u16 + canonical_wire PJ_bytes_view_t`, anchor-ownership rules | The module output contract (§9) mirrors this convention exactly |
| `serializeBuiltinObject` / `deserializeBuiltinObject` (`builtin_object_codec.hpp`) | Host-side decode of module output — the former "object builders" milestone is deleted |
| `min_sdk_required` gating (#164) | parser_extensions and new parser builds gate cleanly on old hosts |

## 5. Architecture overview

```
   author writes ONE source:  class RadarDecoder : pj::ObjectDecoder + PJ_OBJECT_DECODER(...)
        │ native toolchain                          │ wasi-sdk
        ▼                                           ▼
   radar_decoder.so                            radar_decoder.wasm
        └───────────────┬───────────────────────────┘
                 <plugins dir>/decoders/
                        │  scan + manifest read
        ┌───────────────▼────────────────────────────────────────┐
        │ parser_extensions (official plugin, kObjectDecoderProvider) │
        │   dlopen loader │ wasmer loader → one internal contract │
        └───────────────┬────────────────────────────────────────┘
                        │ register claims (PJ_object_decoder_registry_v1)
        ┌───────────────▼───────────────┐      lookup at schema bind
        │ Host: ObjectDecoderRegistry   │◀────────────────────────────┐
        └───────────────────────────────┘                             │
                                                    parser_ros / parser_protobuf
  Per-schema bind: per-topic pin → built-in table → registry candidates
                   (priority order, ACCEPT/DECLINE iteration) → generic flatten
  Per message:     parser ──(event tape | raw payload)──▶ module
                   parser ◀─(canonical PJ.* wire [+ splice ref])── module
```

## 6. Authoring SDK — `pj_base/decoder/`

Header-only subtree inside pj_base, exported as INTERFACE target
`plotjuggler_sdk::decoder` (include paths only — a module links **nothing**):

```
pj_base/include/pj_base/decoder/
  tape.hpp            token codec for binding/event tapes (shared with host side)
  object_wire.hpp     canonical PJ.* wire writer (hand-rolled, no protobuf dep)
  object_fields.hpp   per-type wire field tables + splice-eligible field table
  object_decoder.hpp  pj::ObjectDecoder, pj::EventView, pj::ObjectWriter
  module_export.hpp   PJ_OBJECT_DECODER macro — per-target export glue
```

Constraints (structural, not conventions):

- **wasi-clean**: compiles under wasi-sdk; no exceptions (`Status`/`Expected`), no
  host-only includes, no pointer-width assumptions (POD layouts pinned by
  `static_assert`s against 32/64-bit drift).
- **CI gate**: a job compiles the subtree standalone under wasi-sdk; this replaces the
  isolation a separate package would have provided.
- Host-side code (tape emitters, DecoderLimits validation, splice) lives in normal
  pj_base/pj_plugins and includes the same `tape.hpp`/`object_fields.hpp` — one source
  of truth, adjacent to `pj_base/builtin/` where the canonical structs live.

Author experience (complete):

```cpp
#include <pj_base/decoder/object_decoder.hpp>

class RadarDecoder : public pj::ObjectDecoder {
public:
  pj::Status bind(const pj::BindingInfo& info) override {
    frame_ = info.fieldId("header/frame_id");
    width_ = info.fieldId("width");
    payload_ = info.fieldId("payload");
    return pj::Status::ok();
  }
  pj::Status decode(const pj::EventView& in, pj::ObjectWriter& out) override {
    auto cloud = out.pointCloud();                 // typed canonical-wire builder
    cloud.setFrameId(in.string(frame_));
    cloud.setPointStride(16);
    cloud.addField("x", 0, pj::Datatype::kFloat32); /* … y,z,velocity … */
    auto pts = radar::decompress(in.span(payload_), in.u32(width_));
    cloud.setData(pts);                            // owned bytes — serialized into wire
    // passthrough alternative: cloud.setDataFromInput(in.spanRef(payload_));  // §9 splice
    return pj::Status::ok();
  }
private:
  pj::FieldId frame_, width_, payload_;
};
PJ_OBJECT_DECODER(RadarDecoder)
```

Authoring is **two files** — `radar_decoder.cpp` + `radar.decoder.json` — wired by an
SDK CMake helper (`pj_add_decoder(radar SOURCE … MANIFEST … TARGETS native wasm)`).
The macro emits identical `extern "C"` exports on both targets (native:
default-visibility / `.def`-listed on MSVC; wasm: clang `export_name`, audited
post-link); the manifest is embedded by the **build helper**, not the macro:

- **wasm**: an SDK post-link tool appends exactly one validated `pj_decoder_manifest`
  custom section after `wasm-opt`/stripping (a C++ `__attribute__((section))` data
  definition is NOT a reliable custom-section mechanism — it can land as an active
  data segment); the host requires exactly one such section.
- **native**: the same JSON becomes a constexpr byte array behind two trivial C
  getters (`pj_decoder_manifest_size_v1` / `pj_decoder_manifest_copy_v1`) read after
  `dlopen` — trusted native discovery accepts loader initialization; no ELF/Mach-O/PE
  parsing (three security-sensitive parsers for no security gain).

SDK floor is **C++17** with SDK-owned vocabulary types (`pj::Span`, `pj::Expected`,
`pj::Status`, tagged event — no `std::variant`/`vector`/`string` in the SDK core, no
throwing std paths); owned-blob creation is fallible (`out.allocate_blob(size)` →
`Expected`), backed by module-local realloc bump arenas valid until the next call on
the instance. Tapes are **byte records with named offsets** read/written via explicit
little-endian `memcpy` helpers — never `reinterpret_cast` of structs — with a pinned
`static_assert` set (fixed-width types, IEC 559, wasm32 pointer width). The C++
virtuals never cross the boundary — class + trampolines compile together into the
artifact.

## 7. Registry & host wiring

### 7.1 `pj.object_decoder_registry.v1` (standard service, POD throughout)

Defined exactly like existing services: C vtable (ABI-APPENDABLE, `struct_size`),
fat-pointer service, `Traits` wrapper for C++ consumers. Registration side (used only
by parser_extensions today):

```c
typedef struct PJ_decoder_claim_v1 {          /* POD views; host copies at registration */
  uint32_t struct_size;
  PJ_string_view_t parser_encoding;           /* "ros2msg" | "omgidl" | "ros1msg" | "protobuf" */
  PJ_string_view_t wire_encoding;             /* "cdr" | "ros1" | "" = any for this parser */
  PJ_string_view_t type_name;                 /* normalized (§7.3) */
  PJ_string_view_t schema_digest;             /* optional hex sha256; "" = any */
  PJ_string_view_t decoder_id;                /* unique within the module */
  uint16_t object_type;                       /* frozen BuiltinObjectType; validated */
  uint8_t  mode;                              /* raw | structured */
  int32_t  priority;
  PJ_string_view_t const* field_mask;         /* structured only */
  uint64_t field_mask_count;
  uint16_t min_event_tape, max_event_tape;    /* §10 version negotiation */
} PJ_decoder_claim_v1;

/* vtable slots (sketch — frozen at M1):
   register_claim(ctx, claim*, decoder_iface*, out_handle*, out_error*) -> bool
   unregister_claim(ctx, handle) -> void            // idempotent
   candidates(ctx, key*, out_iter*, out_error*)     // parser side, priority-ordered   */
```

- The host copies every view at registration (nothing borrowed survives the call) and
  returns a `u64` claim handle. parser_extensions holds its claims; the host holds a
  DSO lease on parser_extensions until every decoder instance is destroyed; shutdown
  order: parsers unbind → instances destroyed → claims unregistered → provider
  destroyed.
- `decoder_iface` is a fat pointer to the internal normalized decoder contract (§8) —
  implemented by parser_extensions' two loaders, never by modules directly.

### 7.2 Selection policy (per topic, at schema bind)

1. **Per-topic pin** (`object_decoder_overrides: {"<topic>": "<module>/<decoder_id>"}`
   in parser config, set via parser options UI) — the only override of built-ins.
   **A failing pin fails closed**: object route omitted with diagnostics, scalar route
   continues; falling back to the built-in requires the user removing the pin.
2. **Built-in handler table** — exact normalized encoding + name, as today.
3. **Registry candidates** — ordered by: exact-wire match over `wire_encoding=""`
   wildcard, then priority desc, then `(module_id, decoder_id)` lexicographic (ties
   emit a one-time ambiguity diagnostic). The parser iterates `create`+`bind`:
   **ACCEPT** binds; **DECLINE** (schema digest mismatch, unsupported constructs,
   version window miss) tries next; **ERROR** is diagnosed and tries next. One summary
   diagnostic lists all rejections.
4. **Generic scalar flatten** — unchanged.

### 7.3 Identity & binding context

- Lookup key: `(parser_encoding, wire_encoding, normalized type_name)`. ROS
  normalization (SDK-defined): canonical `pkg/msg/Type`; `pkg/Type` and
  `pkg::msg::Type` fold into it.
- Parsers obtain the registry and a **binding context** (`pj.parser_binding_context.v1`
  → `{parser_encoding, wire_encoding, topic}` for the instance) from the
  `ServiceRegistry` received in `bind()`. Lifetime: services are host-owned and outlive
  the plugin (existing contract); the parser copies context strings during `bind()`.
  Classification-only instances resolve a no-op write service.
- **Classification lifecycle** (v2 finding 19): delegate selection runs once, at the
  first operation that needs it (classify or bind), using the same inputs
  (context + parser config + schema + candidate iteration), and the accepted binding is
  cached and reused — preflight and real binding cannot diverge. A later
  `loadConfig()` that changes wire encoding or pins invalidates the cache, re-runs
  selection, and the parser signals reclassification to the host.

## 8. Decoder contract (internal normalized form + module ABI)

### 8.1 Operations

`create → bind → (decode_raw | decode_tape)* → destroy`, with `output` and
`last_error` accessors. All calls on one instance are serialized by the parser;
distinct instances may run concurrently (parser_extensions guarantees this via
store-per-instance for wasm, §11.3).

### 8.2 Result protocol (v2 finding 21)

`create`/`bind` return `int32`: `0 = ACCEPT`, `1 = DECLINE`, `< 0 = ERROR` — with
`PJ_error_t* out_error` populated on ERROR (module-scope, not handle-scope, so
create-failure is reportable). On DECLINE/ERROR after a successful `create`, the
caller invokes `destroy`; a module must tolerate `destroy` at any lifecycle point.
`decode_*` return `0 = OK`, `< 0 = ERROR` (per-message; never DECLINE).

### 8.3 Module export surface

Native and wasm export **literally identical scalar signatures**, built on a 64-bit
address token (`pj_addr_t = u64`: native = `uintptr_t` round-trip; wasm = zero-extended
linear-memory offset, host-verified to fit u32) and `u32` lengths. Handles are the
module's `Instance*` as u64 — no index table, no global lock. Results travel as
byte-encoded records at caller-supplied addresses (fixed 40-byte output descriptor:
`{descriptor_size, flags, tape_addr, tape_len, arena_addr, arena_len, generation}`),
never as C structs by value. Modules export SDK wrappers `pj_decoder_alloc`/`_free`
(libc `malloc`/`free` names are not part of the ABI). Export set:
`pj_decoder_abi_version`, `pj_decoder_alloc`, `pj_decoder_free`,
`pj_decoder_create(out_handle_addr)` (null handle = allocation failure; fallible init
belongs in `bind`), `pj_decoder_bind`, `pj_decoder_decode_raw`,
`pj_decoder_decode_tape`, `pj_decoder_output`, `pj_decoder_last_error(dst, cap)`
(returns required length; not NUL-terminated), `pj_decoder_destroy`, the manifest
getters (§6, native), plus wasm: `memory`, `_initialize` (wasip1 **reactor** model,
`-mexec-model=reactor`; called exactly once — never also `__wasm_call_ctors`).
Literal signatures freeze in one ABI header at M1; parser_extensions validates every
export's exact signature at load and rejects modules with a wasm start section or
`_start` (a start section executes at instantiation, outside the budgeted init path).

### 8.4 Structured input: binding tape + event tape

Unchanged from v2 in vocabulary (BEGIN_MSG/END_MSG, BEGIN_ARRAY/END_ARRAY, SCALAR,
STRING, PACKED_ARRAY, BYTES_SPAN, TIMESTAMP_NS, DURATION_NS, END_TAPE) with these
bindings tightened (v2 findings 5/6):

- **Sequence elements**: `BEGIN_ARRAY(field_id, count)` then per element
  `BEGIN_MSG(field_id)…END_MSG` (messages) or value tokens carrying the sequence's
  `field_id` (primitives/strings). Empty messages are legal (`BEGIN_MSG`+`END_MSG`
  adjacent); an empty projection yields an empty per-message tape.
- **Presence**: absent optional field ⇒ no token. IDL optionals/unions, protobuf
  maps/oneofs/groups, recursive types, wstrings, multidim arrays ⇒ bind **DECLINE**
  when reachable through the mask; `"*"` over a message containing any of them is a
  hard DECLINE (raw mode is the escape hatch).
- **PACKED_ARRAY** only for a single contiguous fixed-width little-endian wire
  segment (CDR primitive arrays in LE payloads; protobuf packed fixed32/64/float/
  double). Multiple wire segments, varint packing, unpacked repeats, BE payloads ⇒
  materialized `SCALAR`s inside `BEGIN_ARRAY`/`END_ARRAY`. Wire order preserved;
  protobuf duplicate-singular = every occurrence emitted, consumer applies last-wins;
  enums as int32.
- **Field mask grammar** (v2 finding 27): '/'-joined names; selecting a non-leaf
  selects its whole subtree; duplicates deduped; `"*"` = root subtree. Caps applied at
  registration/bind before any allocation: ≤ 256 paths, ≤ 512 bytes/path, ≤ 4096
  expanded entries (host defaults; configurable downward).

### 8.5 Offset spaces (v2 finding 24 — one rule)

All span offsets in tapes are **input-space**: offsets into the payload view **as
presented to the module**. Native modules see the original payload (identity). WASM
modules see a compacted arena containing only the projected span ranges; the host
keeps the inverse segment table. Any input-space range a module returns (splice, §9)
is accepted only if it maps wholly into one contiguous original-payload range —
guaranteed for ranges within one projected segment, a validation error otherwise.

## 9. Module output: canonical wire + splice

Adopting the landed `pj.parser_functional.v1` convention (v2 findings 7/22 resolved by
reuse — the nested-output-model decision is honored by protobuf wire's natural
nesting):

```
output bundle: { object_type u16,            // must equal the claim's — validated
                 canonical_wire view,        // stable PJ.* wire for that type
                 splice_ref? {field u16, input_off u32, len u32} }
```

- **Owned bulk data** (decompressed): serialized inside `canonical_wire` by
  `ObjectWriter` — the one honest copy.
- **Passthrough bulk data**: `canonical_wire` elides the bulk field;
  `splice_ref` names it (validated against the SDK's per-type splice-eligible table:
  `PointCloud.data`, `Image.data`, `CompressedPointCloud.data`, `VideoFrame.data`) and
  gives an input-space range. The host resolves it through §8.5 to the original
  payload and attaches the existing `PayloadView.anchor` — zero-copy end to end; with
  an empty anchor the host materializes a copy (existing parser contract).
- Host decode: `deserializeBuiltinObject(object_type, wire)` + splice + DecoderLimits
  semantic validation (stride/step/dimension invariants). The internal contract exposes
  the result as a **`with_output(consumer)` transaction**, not borrowed spans: the
  loader adapter fetches the descriptor, bounds-checks both spans (wasm: under the
  instance lock, memory base re-acquired), and invokes the host's validator/decoder
  synchronously inside the callback — no module pointer ever escapes the transaction.
  Everything the host retains is copied before the callback returns.
- Scalar output from modules: deferred (unchanged); the bundle has no scalar section.

## 10. Versioning & negotiation (v2 finding 23)

- Module ABI version (`pj_decoder_abi_version` = 1) covers the export surface and
  bundle layout. Tape formats carry `tape_version`; claims declare
  `[min_event_tape, max_event_tape]`; the parser picks the highest mutually supported
  version at bind or DECLINEs. Canonical wire is versioned upstream by the SDK
  (`PJ.*` contract) — modules emit the version they were built against; the codec's
  own compatibility rules apply. Unknown tokens/fields in v1: reject, never skip.
- One compatibility matrix (ABI 1 ⇔ event tape 1 ⇔ binding tape 1) ships in the ABI
  header and is extended, never rewritten.

## 11. parser_extensions (official plugin)

### 11.1 Loading

Scans `<plugin dir>/decoders/` at startup (rescan requires restart, v1). Native `.so`
loaded with `dlopen(RTLD_LOCAL | RTLD_NOW)` (Windows: `LoadLibraryExW` with
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, absolute path)
— loaded by default: placing the file is the trust act. Module hygiene: hidden
visibility with only `pj_decoder_*` exported, linked with no unresolved symbols
(`-z,defs`), symbols resolved per-handle (identical names across modules are fine
under RTLD_LOCAL), no substantive `DllMain`/global-ctor work. **v1 never unloads a
successfully loaded native module during the session** — eliminating shutdown-order,
TLS-destructor, and dangling-pointer hazards outright. `.wasm` via wasmer (statically
linked, prebuilt, pinned build — the metering C API is unstable, so the pin is
load-bearing). Wasm manifests are read from the custom section before any module code
runs; native manifests via the C getters after dlopen (§6).

### 11.2 Manifest (JSON, size-capped)

```json
{ "abi_version": 1, "name": "radar-decoders", "version": "1.0.0",
  "decoders": [{
    "decoder_id": "radar-scan-v1",
    "parser_encoding": "ros2msg", "wire_encoding": "cdr",
    "type_name": "my_msgs/msg/RadarScan",
    "object_type": "kPointCloud",              // canonical PJ::sdk::name() strings
    "mode": "structured", "priority": 0,
    "event_tape": [1, 1],
    "field_mask": ["header", "width", "payload"] }] }
```

### 11.3 WASM execution model

Compiled module cached and shared via the C API's shared-module mechanism
(`wasm_module_share`/`obtain` per store); **one store + instance per bound decoder**
(no cross-topic head-of-line blocking). *M4 gate: prototype share/obtain against the
pinned wasmer 7 build before freezing; fallbacks are serialized compiled artifacts,
per-module store + mutex, or a small instance pool.* Store **thread affinity** must be
verified — "calls serialized" is insufficient if a store cannot migrate OS threads;
if needed, wasm calls route through per-store executor workers. Structured mode copies
tape + projected spans into a compact guest arena tracked by an explicit segment table
`{guest_start, length, original_host_start, span_id}`; `EventView` blob accessors carry
the `span_id`, and a returned splice ref is accepted only against an authorized
segment (§8.5). Memory base re-acquired after **every** guest call — including
`pj_decoder_alloc`/`_free` and `_initialize`, which can themselves grow memory (the
PJ3 prototype's cached pre-decode output pointer is exactly this bug). Instantiation
is **lazy** (first bind) with admission control against the budgets below; per-decoder
instance cost is benchmarked at 10/100/1000 bound topics.

### 11.4 Hardening & budgets (v2 findings 8/13/25/26)

- Imports: exact allow-list; anything else rejects the module. No stdin; no fs/net.
  Export signatures validated; wasm **start sections and `_start` rejected**; command
  modules rejected (reactor only). Guests build `-fno-exceptions`; traps are the error
  path. Module compilation itself is bounded (size/function caps checked pre-compile,
  compiled off the UI thread) — runtime fuel does not protect compilation.
- Per-call: wasmer metering/epoch deadline, memory-page cap, stack cap.
- **Aggregate (session)**: max modules, max module file size (checked before
  compilation), max total claims, max active instances, total linear-memory budget.
  Admission failure = bind DECLINE with diagnostic.
- **Quarantine separates faults from data errors**: ordinary `decode` errors
  (bad payload) are per-message diagnostics only — never strikes. Runtime faults
  (trap, deadline, memory exhaustion) and contract violations (malformed bundle,
  invalid splice) strike the `(module, decoder_id)` key; N=3 strikes in a session
  quarantines it: instance destroyed, recreated on next use via the full
  `create`+`bind` replay from cached inputs; a second quarantine disables the decoder
  for the session with a summary diagnostic.

## 12. Failure semantics (parsers)

Scalar and object routes stay independent (lazy object route): decode failure omits
the object; committed scalar rows remain; no transactional rollback; diagnostics
rate-limited per topic. No auto-demotion to generic flatten. Pin failures: §7.2,
fail-closed.

## 13. Performance obligations

Structured emitters: CDR emitter over `RosMsgParser::Deserializer` (M2); protobuf
requires a **new descriptor-aware wire scanner** — DynamicMessage preserves no offsets
(M3). Benchmarks are deliverables with regression budgets fixed from M1/M2
measurements: baseline generic flatten vs structured delegation vs raw delegation;
wasm-vs-native decode overhead on a 1MB cloud fixture. No unmeasured claims.

## 14. Testing strategy

- **SDK**: registry service tests (registration copies, handles, leases, shutdown
  order, candidate ordering incl. wildcard specificity, ACCEPT/DECLINE/ERROR
  iteration); tape round-trips; DecoderLimits adversarial corpus + fuzz targets (tape
  validators, manifest parser, splice validation); canonical-wire writer golden tests
  against `deserializeBuiltinObject`; wasi compile gate + POD layout static_asserts.
- **Parsers**: golden event tapes from CDR/protobuf fixtures (subset DECLINEs, packed
  vs materialized, BE payloads, empty projections, sequence-of-message delimiting);
  selection-policy tests incl. pin fail-closed and config-change reclassification;
  scalar-route regressions.
- **parser_extensions**: fixture modules built from one source as both `.so` and
  `.wasm` (source committed; prebuilt artifacts committed, wasi-sdk in CI as
  follow-up): raw, structured, splice passthrough, trap, deadline loop, memory bomb,
  malformed bundle, bad manifest, duplicate manifest sections, start-section module,
  disallowed import, admission-limit hits, quarantine strike/replay. Post-link ABI
  audit (exact export set, no extras) and manifest-embedder round-trip tests.
- **E2E**: pj_proto_app smoke — MCAP with custom type + the example module (both
  formats), screenshot-verified cloud.

## 15. Milestones — vertical slice

| # | Deliverable | Repo | Depends |
|---|---|---|---|
| M1 | `pj_base/decoder/` subtree (tape codec, canonical-wire writer, ObjectDecoder/EventView/ObjectWriter, native macro target) + registry service + provider family + leases + binding-context service + DecoderLimits + wasi CI gate; parser_ros fallback (raw mode, native modules) end-to-end; benchmark baseline | plotjuggler_sdk + parser_ros | — |
| M2 | Structured CDR: emitter, projection, subset DECLINEs; splice; classification-lifecycle wiring | SDK + parser_ros | M1 |
| M3 | Structured protobuf: descriptor-aware wire scanner + parser_protobuf integration | pj-official-plugins | M1 (M2 formats) |
| M4 | parser_extensions wasm loader: hardening, budgets, quarantine, wasm macro target + custom-section manifest, offset remap; E2E both formats | pj-official-plugins | M1–M2 |
| M5 | Pin UI, template repo (one source, two build presets), authoring docs | plugins + new repo | M1–M4 |

Host registry ownership, provider bootstrap, and classification integration are M1
scope. `deserializeBuiltinObject` reuse removes the former object-builder milestone.

## 16. Deferred / rejected

**Rejected:** Lua; declarative mapping/schema hints; host-owned WASM runtime;
silent priority override of built-ins; manifest-by-execution for wasm; bespoke nested
output format (superseded by canonical-wire reuse).
**Deferred:** native provider plugins as a documented extension path; supplementary
scalars; maps/oneofs/recursion in structured mode; `.wasm` via extension registry;
vendored single-header authoring kit (build-without-SDK); rescan without restart;
module-supplied options UI; transactional scalar rollback.

## 17. Key decisions log

1. **Harmonized decoder modules** — one C++ source, two build targets, one folder, one
   host plugin; format choice is the user's, never the architecture's (maintainer,
   2026-08-08).
2. **Authoring API in `pj_base/decoder/`** — no new package; INTERFACE target +
   wasi CI gate carry the constraints (maintainer, 2026-08-08).
3. **Output = canonical PJ.* wire + splice** — reuse of #166/#168 contracts; the
   nested-output decision honored through protobuf wire's natural nesting.
4. **Registry/binding delivery via existing `ServiceRegistry`** — zero parser
   base-class change; sentinel tests stay green by construction.
5. **Structured subset with hard DECLINE**; raw mode escape hatch (maintainer).
6. **Per-topic pin, fail-closed** — the only built-in override.
7. **Faults strike, data errors don't** — quarantine reserved for traps/deadlines/
   contract violations.
8. **Availability limits are v1 prerequisites**, per-call and aggregate.
9. **wasmer, prebuilt; store-per-instance; lazy instantiation with admission.**
10. **Vertical-slice milestones** (maintainer).
