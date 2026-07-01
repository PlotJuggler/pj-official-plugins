# Design: `data_load_mf4` — ASAM MF4 / MDF4 DataSource plugin

**Date:** 2026-07-01
**Branch / worktree:** `feat/mf4-plugin` (`.worktrees/feat-mf4-plugin`)
**Status:** Approved design → planning

## 1. Summary

A new PlotJuggler **file DataSource** plugin (`PJ::FileSourceBase`) that imports ASAM
Measurement Data Format v4 (`.mf4`, also `.mdf`/`.dat`) files. Backed by
[`ihedvall/mdflib`](https://github.com/ihedvall/mdflib) (MIT) for MDF parsing and
[`dbcppp`](https://github.com/xR3b0rn/dbcppp) (MIT) for optional DBC-based CAN
signal decoding.

**This is a net-new plugin, not a port.** MF4 is not one of the 12 original
PlotJuggler plugins, so `porting_guide.md`'s "every original code path must have a
corresponding path" has no upstream baseline. This spec is the baseline the
implementation is audited against.

## 2. Scope (v1) — confirmed decisions

| Decision | Choice |
|---|---|
| Channel types | **Numeric scalars + strings + enums** (arrays/CA deferred) |
| CAN/LIN decode | **DBC-based CAN decode in v1** (LIN deferred) |
| UI | **Channel-selection dialog** (+ DBC file picker) |
| Testing | **Both** synthetic (mdflib writer) + committed real sample |

**In scope:** finalized MDF4 read; numeric channels with CC (engineering values);
string/text channels; value→text enum channels; per-channel-group timelines;
CAN bus-logging groups decoded via user-supplied DBC(s); channel/group selection
dialog; progress + cancellation; robustness on unfinalized (`UnFinMF`) files.

**Out of scope (v1):** multidimensional channel arrays (CA blocks); LIN/FlexRay/
Ethernet decode; writing/editing MF4; MDF v3-specific features beyond what mdflib
transparently handles; A2L.

## 3. Architecture — module decomposition

Each unit is independently testable with one clear responsibility.

| Unit | Responsibility | Depends on |
|---|---|---|
| `mf4_source.{hpp,cpp}` | `FileSourceBase` subclass; orchestrates open→select→import; progress, errors, config | reader, value, can_decoder, dialog |
| `mf4_reader.{hpp,cpp}` | mdflib wrapper: `ReadEverythingButData` → channel index; read a group's rows via observers | mdflib |
| `mf4_value.hpp` | mdflib `ChannelDataType` → `PJ::PrimitiveType`; read one sample → `ValueRef` (numeric/string/enum) | mdflib |
| `can_decoder.{hpp,cpp}` | Load .dbc; decode `(id, bytes)` → named signals; DBC-library-agnostic interface | dbcppp |
| `mf4_dialog.{hpp,cpp}` + `mf4_dialog.ui` | Channel-group/channel selection tree + DBC file picker(s) | SDK dialog (Qt→header) |
| `mf4_manifest.hpp` / `manifest.json` | Register `.mf4/.MF4/.mdf/.dat` | — |

Mirrors `data_load_ulog` structure: `mf4_reader`≈`ulog_flatten`,
`mf4_value`≈`ulogTypeToPrimitive`/`readPrimitiveValue`, `mf4_dialog`≈`ulog_params_dialog`.

**Key isolation:** `mf4_source` never sees dbcppp types — it passes `(id, bytes)` to
`can_decoder` and receives named signals. Keeps the riskiest dependency swappable
(dbcppp ↔ `dbc_parser_cpp`) and unit-testable without any MF4 file.

## 4. Data flow (`importData`)

1. Open with mdflib `MdfReader`. Detect unfinalized (`UnFinMF`) via
   `MdfFile::IsFinalized()`; finalize/handle before reading (see Risks).
2. `ReadEverythingButData()` → build lightweight channel index (group name, channel
   name/unit/type/master flag/sample count, `GetBusType()`).
3. Selection: from saved config (`loadConfig`) or the dialog — which groups/channels,
   and any DBC files for CAN groups.
4. **Measurement groups:** create observers for master + selected channels →
   `ReadData(dg)` → per record, assemble a row and
   `appendRecord(topic=group_name, ts, fields=channels)`. One row per record (all
   channels in a group share the master axis — matches ulog/driveline).
5. **CAN bus groups** (`GetBusType()==CAN`) with a DBC: `CanBusObserver` → each
   `CanMessage` → `can_decoder.decode(id, bytes)` → emit decoded signals as series at
   the frame timestamp. Unmatched frames counted + summarized (no silent drop).
6. Progress by file size; honor `isStopRequested()`; `reportMessage` summary.

**Timestamp policy:** absolute `int64` ns = HD start time (ns since epoch) +
per-sample master offset. Consistent with the ULog/MCAP sources.
**Value semantics:** engineering values (`GetEngValue`, CC applied) for numerics;
text channels as strings; value→text CC as strings.

## 5. Error handling

- Non-MDF / unreadable file → `PJ::unexpected` with specific message.
- Unfinalized but unreadable → clear error naming the file (after finalize attempt).
- mdflib parse errors → surfaced via `reportMessage` / `unexpected`.
- Untypeable / unaddressable channels → skipped with a **counted warning** (no silent
  drop; mirrors driveline's `unaddressable_dropped` and ulog's null-on-truncation).
- CAN frame with no DBC match → counted + one summary message.
- Cancellation honored via `isStopRequested()`.

## 6. Dependencies & build

- **mdflib** via **CPM** (no Conan recipe), core `mdflib` target only
  (`MDF_BUILD_*` viewer/csv/python/.NET OFF). Feed **zlib + expat from Conan** so it
  does not fetch its own. MIT.
- **dbcppp** via **Conan** (conancenter recipe exists; pulls header-only Boost.Spirit).
  MIT. Fallback: `LinuxDevon/dbc_parser_cpp` if Boost is unwanted.
- **Qt6** only for the dialog `.ui`, compiled to a `constexpr` header by
  `pj_embed_ui()` — **no Qt build dep on the plugin `.so`** (per CLAUDE.md dialog
  system).
- Self-contained artifact: static-link, hidden visibility, `RTLD_LOCAL` per repo rule
  (`[[feedback_plugin_self_contained_artifacts]]`). `CMakeLists.txt` mirrors ulog's;
  `conanfile.py` adds dbcppp + zlib + expat.

## 7. Testing

- **Synthetic unit tests** via mdflib `MdfWriter` (like the mf4-rs tests): single
  scalar group; multi-rate groups; string/enum channel; shared-name masters. Assert
  topics/fields/timestamps/values through a capturing test host (as
  `ulog_source_test.cpp` does).
- **`can_decoder` unit test:** inline DBC text + crafted frames → assert decoded
  signals. Fully hermetic.
- **Integration smoke tests:** a small committed real `.mf4` (trimmed ASAP2 demo) →
  assert channel count + a known signal; plus a trimmed unfinalized CANedge file for
  the raw-CAN + finalization path.
- Fixtures live under `data_load_mf4/test_data/` (mirrors ulog's `test_data/`).

## 8. Open risks (resolve during planning/impl)

1. Does mdflib read `UnFinMF` directly, or must we finalize a temp copy first?
2. mdflib observer lifecycle / memory for large multi-GB files (whole-group read).
3. Dialog population cost for files with thousands of channels (lazy vs eager).
4. mdflib CPM integration: does its CMake cleanly consume Conan-provided zlib/expat,
   or does it need a small patch / `find_package` shim?
5. dbcppp Conan recipe availability across the repo's platforms (Linux primary;
   macOS CI).

## 9. Sample fixtures already obtained

- `ASAP2_Demo_V171.mf4` (1.2 MB, finalized, named scalar signals + CC) — clean case.
- `canedge_00000001.MF4` (3.1 MB, `UnFinMF`, raw CAN) — unfinalized + bus-logging case.
