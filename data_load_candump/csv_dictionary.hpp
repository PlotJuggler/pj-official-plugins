#pragma once

// Translates an ARUS-style CSV CAN dictionary (header "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name") into DBC
// text for pj_can_dbc::CanDecoder::loadDbcString(), so CSV and .dbc dictionaries share one decode path.
// Semantics follow ARUSfs/log_plotter (read_can_txt_file, can_conversions.csv):
//   - bitIn/bitFin are INCLUSIVE BYTE indices (not bit indices), always little-endian.
//   - phys = raw * Scale + Offset. `Power` is present in the CSV but unused.
//   - Signed accepts True/False, 1/0, yes/no, case-insensitive.
//   - An ID of exactly 4 hex digits is base (first 3 digits) + a per-row signal index (the 4th digit, dropped when
//     rows are grouped by base). Any other digit count is a single-signal message with that id.
//   - Rows sharing a base id become one BO_ message with one SG_ per row; DLC = max bitFin + 1; a base id above
//     0x7FF is emitted as extended (Vector convention: BO_ id | 0x80000000, as pj_can_dbc::CanDecoder expects).

#include <pj_base/expected.hpp>
#include <string>

namespace candump_detail {

/// Invalid rows (non-hex ID, bitIn > bitFin, bitFin > 7, an unparseable Signed/Scale/Offset) are skipped with one
/// line appended to `warnings` each. Returns an error only for a missing required header column or no valid rows.
PJ::Status arusCsvToDbc(const std::string& csv_text, std::string& out_dbc_text, std::string& warnings);

/// Classic-locale decimal text with the smallest precision in [15, 17] significant digits that parses back to
/// exactly `value`. May contain a negative sign and exponential notation, which the vendored dbc.cpp accepts.
std::string formatDbcNumber(double value);

}  // namespace candump_detail
