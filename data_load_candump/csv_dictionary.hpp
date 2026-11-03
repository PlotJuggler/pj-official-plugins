#pragma once

// csv_dictionary: translates an ARUS-style CSV CAN dictionary
// (header "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name") into DBC text
// consumable by pj_can_dbc::CanDecoder::loadDbcString() -- so the CSV
// dictionary and a real .dbc file are decoded through the exact same path.
//
// Semantics verified against ARUSfs/log_plotter's read_can_txt_file and the
// real https://github.com/ARUSfs/log_plotter/blob/main/can_conversions.csv:
//   - bitIn/bitFin are INCLUSIVE BYTE indices (not bit indices) into the CAN
//     payload, always little-endian.
//   - phys = raw * Scale + Offset. `Power` is present in the CSV but unused.
//   - Signed is a bool (accepts True/False, 1/0, yes/no, case-insensitive).
//   - An ID written with exactly 4 hex digits is ID-base (its first 3 hex
//     digits) + a per-row signal index (the 4th digit, discarded once rows
//     are grouped by base -- it exists only so each row's ID column is
//     distinct for a multi-signal message). Any other digit count is used
//     as-is: a single-signal message with that id.
//   - Rows sharing the same base id become one BO_ message with one SG_ per
//     row; DLC = (max bitFin across the message's signals) + 1; a base id
//     above 0x7FF (11-bit standard range) is emitted as extended (Vector
//     convention: BO_ id | 0x80000000, matching pj_can_dbc::CanDecoder's own
//     extended-frame lookup).

#include <pj_base/expected.hpp>
#include <string>

namespace candump_detail {

/// Converts `csv_text` (already read into memory) to DBC text ready for
/// CanDecoder::loadDbcString(). Invalid rows (non-hex ID, bitIn > bitFin,
/// bitFin > 7, an unparseable Signed/Scale/Offset) are skipped -- one
/// warning line appended to `warnings` per skipped row -- rather than
/// failing the whole conversion. Returns an error only when the header
/// itself cannot be parsed (missing ID/bitIn/bitFin/Signed/Scale/Offset/Name
/// columns) or the input has no data rows at all.
PJ::Status arusCsvToDbc(const std::string& csv_text, std::string& out_dbc_text, std::string& warnings);

/// Minimal-round-trip decimal formatting of a DBC factor/offset number:
/// classic ('.' decimal point, no grouping) locale, the smallest precision
/// in [15, 17] significant digits whose text parses back to exactly `value`
/// (falls back to 17, which always round-trips a double). The vendored
/// dbc.cpp fix (common/can_dbc) accepts a negative sign and exponential
/// notation, so this does not need to dodge either.
std::string formatDbcNumber(double value);

}  // namespace candump_detail
