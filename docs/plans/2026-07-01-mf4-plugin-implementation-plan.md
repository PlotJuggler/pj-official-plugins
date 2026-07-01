# Implementation Plan: `data_load_mf4`

**Date:** 2026-07-01 · **Branch:** `feat/mf4-plugin` · **Spec:** `docs/superpowers/specs/2026-07-01-mf4-plugin-design.md`

## Provenance

Produced by a multi-model fusion-plan council (blinded Opus synthesis); base = winning
**risk-first** draft (A, 31/31). The council's **Codex seat stalled/was skipped**, so the plan
was then handed to **Codex (gpt-5.x) for a dedicated read-only critique** — its corrections are
folded in below (see **Codex corrections** changelog at the end). Council scoreboard: A(claude
risk-first) 31 ⭐ · D(deepseek) 25 · B(claude simplicity) 23 · C(glm-5.2) 21.

---

## Ground-truth API (verified against sources — do NOT re-guess)

**PJ SDK — CONFIRMED by Codex against the headers:**
- Subclass `PJ::FileSourceBase`; override `extraCapabilities()`, `getDialog()`, `saveConfig()`,
  `loadConfig()`, `importData()` (`data_source_patterns.hpp:69-80`).
- Errors `PJ::unexpected(std::string(...))`; success `PJ::okStatus()` (**no `PJ::makeError`**).
- `writeHost()`/`runtimeHost()` (`data_source_plugin_base.hpp:170-176`); `ensureTopic`→check
  `if(!t) return PJ::unexpected(t.error());`; `ensureField(*t,name,PrimitiveType)`;
  `appendRecord(*t, PJ::Timestamp{ns}, Span<const PJ::sdk::NamedFieldValue>)` (keep ulog's explicit
  Span style, `plugin_data_api.hpp:394-404`).
- **`ValueRef` = `std::variant<NullValue,TypedNull,float,double,int8_t..uint64_t,bool,std::string_view>`
  (`plugin_data_api.hpp:83-85`) — NO owning-string alternative.**
- Macros `PJ_DATA_SOURCE_PLUGIN(Class,kManifest)`, `PJ_DIALOG_PLUGIN(DialogClass)`.

**mdflib (pin a verified tag — see Risk #13; local `/tmp/mdflib` is untagged, treat as reference only):**
- `MdfReader r(path)`; `IsOk()`; `ReadEverythingButData()`; `GetFile()`→`const MdfFile*`; `GetHeader()`;
  **`GetStartTime()`→`uint64_t` ns since 1970**; `IsFinalized()`; `ReadData(dg)` **→ materializes the
  whole data-group into memory; call `dg->ClearData()` after each group** (`mdfreader.h:65-78,198-207`).
- `MdfFile::DataGroups(list)`; `IDataGroup::ChannelGroups()`; `IChannelGroup::Channels()/NofSamples()/
  GetBusType()`; `IChannel::Name()/Unit()/Type()(Master|VirtualMaster)/DataType()`.
- `CreateChannelObserverForChannelGroup(dg,cg,ChannelObserverList&)`; read `obs->GetEngValue(i,double&)`
  (CC-applied, returns bool valid) / `obs->GetChannelValue(i,std::string&)` for text; `obs->NofSamples()`.
- **CAN:** `CanBusObserver o(*dg, ...)`; `o.OnCanMessage = [](uint64_t sample, const CanMessage& m){...}`
  (`canbusobserver.h:29-31`); `r.ReadData(*dg)`. Use **`m.CanId()`** (raw 11/29-bit) for DBC match and
  **`m.Timestamp()`** (relative seconds) for time — NOT `MessageId()` (bit31=extended flag)
  (`canmessage.h:55-92,180-181`; `testcanbusobserver.cpp:156-173`; `testbuslogger.cpp:131-134`).
- **Writer (fixtures):** `MdfFactory::CreateMdfWriter(MdfWriterType::MdfConverter)`; `writer->Init(path)`;
  `header->StartTime(ns)`; `CreateChannelGroup(dg)`/`CreateChannel(cg)` (static); `InitMeasurement()`;
  set channel values + `SaveSample(cg, absolute_ns)`; **`StartMeasurement(start_ns)`** … **`StopMeasurement(stop_ns)`**;
  `FinalizeMeasurement()` (`mdfwriter.h:287-291`; copy `mdflib_test/src/testwrite.cpp:180-182` exactly).

**dbcppp — CORRECTED (Codex #8): NOT header-only.** Conan `dbcppp/3.2.6` is a **static library** that
**requires `boost/1.80.0`** and builds with CMake. The boost/1.80 pin risks the repo's known
Arrow↔boost conflict when added to the aggregate conanfile → **strongly prefer `LinuxDevon/dbc_parser_cpp`
(MIT, C++11, no Boost) as the primary CAN parser**; keep dbcppp as fallback. Confirm the chosen lib's
decode API from its headers in Phase 0 (do not cite specific symbols until read).

**mdflib CMake — CORRECTED (Codex #1):** its `CMakeLists.txt:179-190` uses `${CMAKE_SOURCE_DIR}` for
`include(script/{zlib,expat}.cmake)` and `$<BUILD_INTERFACE:.../include>`. Under CPM/`add_subdirectory`
that is the PARENT project → **hard failure**. **Mandatory Phase-1 patch:** rewrite those to
`${CMAKE_CURRENT_SOURCE_DIR}`/`${mdflib_SOURCE_DIR}` (via CPM `PATCH_COMMAND` or a committed `.patch`).
`find_package(ZLIB)`+`find_package(EXPAT)` before the add still needed so the scripts reuse Conan targets.

---

## Hard Rules (non-negotiable)

1. **Strings:** never build `ValueRef` from `std::string`. Keep owning strings in a **row-scoped buffer
   that outlives the `appendRecord` status return** (Codex #10: the C ABI `append_record` does not
   document a copy, unlike `push_owned`), push `std::string_view` into them (as `ulog_source.cpp` does).
2. **Numeric mapping:** ALL numeric channels → `kFloat64` via `GetEngValue(i,double&)` (CC-applied). Text
   → `kString`. **Enum value→text tables OUT of v1 scope.**
3. **Invalid sample:** `GetEngValue/GetChannelValue`→`false` ⇒ push `PJ::NullValue{}` (keep alignment).
4. **Memory (CORRECTED, Codex #4):** bounded **per-data-group**, not per-sample — `ReadData(dg)`
   materializes the whole group. Create observers, read via row callback, then **`dg->ClearData()`** and
   drop observers before the next group. Honor `isStopRequested()` between groups/rows. `ReadPartialData`
   batching is a future large-file phase.
5. **`can_decoder.hpp` dbc-lib-free** (pImpl/forward-decl); grep-verify no `dbcppp`/`dbc_parser` token in
   the header. `mf4_source` never sees a decoder type.
6. **Self-containment:** static-link, hidden visibility, `RTLD_LOCAL`. Audit `nm -D --defined-only` →
   only PJ entrypoints; no `mdf*`/`dbc*`/`boost*`/`zlib`/`expat` leak.
7. **Conversions (BROADENED, Codex #11):** checked `static_cast` helpers at ALL boundaries —
   `std::filesystem::file_size`→`uint64` for `progressStart`, `GetStartTime()`(u64)+`llround`→`int64`
   (overflow-guard), `NofSamples()`(u64) loop bounds, `size_t`↔`uint64`. SYSTEM-wrap mdflib+decoder includes.
8. **Topic + FIELD collisions (EXPANDED, Codex #5):** topic = group name else `Group {dg}/{cg}`, dup→`#{i}`.
   **Also de-dup channel/field names WITHIN a group** (mdflib gives no uniqueness guarantee): field =
   channel name else `chan{c}`, dup→`{name}#{c}`. Fixture must include duplicate names in one group.
9. **CAN ID:** decode input = `m.CanId()`; select 11-bit vs 29-bit via the extended flag; match DBC id on it.
10. **Timestamp:** `int64 ns = static_cast<int64>(reader.GetStartTime()) + llround(sec*1e9)` (measurement:
    master seconds; CAN: `m.Timestamp()`), overflow-guarded.
11. **No-master groups (NEW, Codex #6):** define behavior explicitly — v1 **skips a measurement group with
    no `Master`/`VirtualMaster` channel with a counted warning** (matches driveline); add a synthetic
    fixture for it. (Synthetic index-time is a documented future option.)
12. **Integration test drives `Mf4Reader` directly** on real fixtures — NOT `plugin_host` (no capturing
    WriteHost exists). A full `importData()`-through-DataEngine test is a separate risk-flagged task.

---

## Phase 0 — Build-integration spike (throwaway, gates everything)

1. Scratch `spike/` (git-ignored): `find_package(ZLIB/EXPAT)` → `CPMAddPackage(mdflib @ pinned tag)`
   **with the mandatory `${CMAKE_SOURCE_DIR}` patch** → link `mdf`. Open BOTH `/tmp/mf4_samples/*.mf4`;
   print `IsOk/IsFinalized/GetStartTime`, group/channel counts, a master+value; run `CanBusObserver` on
   the canedge file printing `CanId/Timestamp`.
2. Evaluate the CAN parser: try `dbc_parser_cpp` first; confirm it links with NO boost and read its decode
   API from headers. If unusable, fall back to `dbcppp/3.2.6` and record the boost/1.80 aggregate impact.
3. Read `mdflib_test/src/{testmdfwriter,testwrite,testcanbusobserver}.cpp`; copy exact writer/observer
   call sequences into a scratch note the later phases reuse.
4. **Verify:** spike compiles; both files read without crash; record whether canedge (`UnFinMF`) reads or
   needs finalization; record the exact mdflib CMake patch. Delete spike after.

## Phase 1 — Scaffolding + committed build plumbing (Codex #12: before any tests)

`data_load_mf4/`: `manifest.json` (`.mf4/.MF4/.mdf/.dat`), `conanfile.py` (mirror ulog + chosen
CAN lib + `zlib` + `expat`), `CMakeLists.txt` (mirror ulog: find_package ZLIB/EXPAT/CAN-lib;
`CPMAddPackage(mdflib)` **with patch**; `set_target_properties(mdf … PIC ON,
INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)`; SHARED plugin linking `plotjuggler_sdk::plugin_sdk mdf
<canlib> nlohmann_json`; `pj_embed_manifest`/`pj_emit_plugin_manifest`/`pj_embed_ui`; **test exe with
full test wiring** linking `mdf <canlib> GTest::gtest_main`). Wire into BOTH root CMake blocks + aggregate
conanfile. **Commit this plumbing** (empty `mf4_source.cpp` stub) once the `.so` + empty test build.

## Phase 2 — `mf4_value.hpp` — pure mapping only (Codex #7)

Hermetic: `mf4TypeToPrimitive(ChannelDataType)->PrimitiveType` (numeric→kFloat64, text→kString).
The sample-reading helper is tested in Phase 3 (a real `IChannelObserver` can't be faked from the public
abstract API). **Test** `tests/mf4_value_test.cpp`: every ChannelDataType maps correctly.

## Phase 3 — `mf4_reader.{hpp,cpp}` + sample-read (TDD, synthetic fixtures)

`open`; `index()` (group/channel meta incl. master flag, bus type, no-master flag); `readGroup(dg,cg,
RowCallback(int64 ts,span<NamedFieldValue>))` — observers created+read+freed, `ClearData()` after
(Rule 4); string lifetime via row buffer (Rule 1); invalid→Null (Rule 3); `startTimeNs()`, `finalized()`.
**Tests** `tests/mf4_reader_test.cpp` (writer sequence copied from `testwrite.cpp`): single scalar group;
two rates; a string channel; shared-name masters across groups; **duplicate channel names in one group**;
**a group with no master**. Assert topics/fields/timestamps/values. No SDK host.

## Phase 4 — `can_decoder.{hpp,cpp}` (TDD, hermetic)

Chosen CAN lib behind pImpl (Rule 5). `loadDbc(path)`; `decode(uint32 canId, bool extended,
span<const uint8>)->vector<DecodedSignal{name,value,unit}>` (Rule 9). **Test** inline DBC + crafted frame
(`0x03E8`→`100.0`), assert name/value/unit. Zero MF4 files.

## Phase 5 — `mf4_source.cpp` (orchestrator)

`importData()`: open; `!finalized()`→warn+attempt (per Phase 0 finding); progress via
`std::filesystem::file_size`; selected measurement groups→`readGroup`→`appendRecord`; selected CAN groups
with a DBC→`CanBusObserver` (time from `m.Timestamp()`, id `m.CanId()`)→`can_decoder`→emit; count unmatched
frames+summary; `isStopRequested()`. `save/loadConfig` persist filepath+selection+dbc paths (nlohmann_json).

## Phase 6 — `mf4_dialog.{hpp,cpp}` + `.ui`

`PJ::DialogPluginTyped`, `buttonBox` named `buttonBox`. Group→channel checkbox tree populated **eagerly
from metadata** (lazy-expand = documented future opt). DBC file picker list. `PJ_DIALOG_PLUGIN`.

## Phase 7 — Integration smoke tests (real fixtures, drive `Mf4Reader`)

`MF4_TEST_DATA_DIR`; **trim** committed fixtures small (verify sizes). Assert ASAP2 demo channel count + a
known signal; canedge reads (finalization path) + ≥1 CAN group decoded with a tiny DBC.

## Phase 8 — Hardening + verify

`nm -D` symbol audit (Rule 6); ASAN (`build.sh --debug`) + `-Werror` clean; `pj_proto_app` screenshot
smoke on ASAP2 demo; `ctest -R mf4` green.

## Risk register

| # | Risk | Mitigation | Phase |
|---|---|---|---|
| 1 | mdflib `${CMAKE_SOURCE_DIR}` breaks under CPM | **mandatory** patch → `${mdflib_SOURCE_DIR}` | 0/1 |
| 8 | dbcppp static-lib pins boost/1.80 → aggregate Arrow conflict | prefer `dbc_parser_cpp` (no boost); else exclude from aggregate | 0/1 |
| 4 | `ReadData` materializes whole group (>1GB warn) | per-group read + `ClearData()`; ReadPartialData later | 3/5 |
| 2 | CAN timestamp | `m.Timestamp()` + start time | 5 |
| 3 | CAN id extended flag | `m.CanId()` + extended flag | 4/5 |
| 5 | field-name collisions in a group | per-topic field de-dup + fixture | 3 |
| 6 | groups with no master | skip + counted warning + fixture | 3/5 |
| — | unfinalized `UnFinMF` read | spike records; warn+attempt+clean error | 0/5 |
| 7 | observer not fakeable | value-read tests in Phase 3 (real fixtures) | 2/3 |
| 6 | symbol/boost leakage | `nm -D` audit + pImpl | 4/8 |
| 13 | mdflib version pin | pin tag **+ archive SHA**; local clone ≠ proof | 0/1 |

## Codex corrections folded in (changelog)

1 mdflib CMake patch made mandatory · 2 CAN time via `Timestamp()` not master-observer · 3 `CanId()` not
masked `MessageId()` · 4 memory bounded per-group + `ClearData()` (not per-sample streaming) · 5 field-name
de-dup within group · 6 no-master group behavior defined · 7 observer sample tests moved to Phase 3 · 8
dbcppp = static lib + boost/1.80 → `dbc_parser_cpp` preferred · 9 `StartMeasurement(start_ns)` arg · 10
string lifetime through appendRecord return · 11 broadened `-Wconversion` discipline · 12 commit build
plumbing before tests · 13 pin mdflib tag+SHA. SDK/writeHost/ValueRef/appendRecord shapes CONFIRMED correct.
