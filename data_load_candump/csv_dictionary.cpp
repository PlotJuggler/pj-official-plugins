#include "csv_dictionary.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <map>
#include <pj_base/number_parse.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_can_dbc/can_topic.hpp>
#include <sstream>
#include <vector>

#include "candump_parser.hpp"

namespace candump_detail {

namespace {

struct CsvRow {
  int bit_in = 0;
  int bit_fin = 0;
  bool is_signed = false;
  double scale = 0.0;
  double offset = 0.0;
  std::string name;
};

std::string trim(std::string_view s) {
  std::size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
    ++start;
  }
  std::size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
    --end;
  }
  return std::string(s.substr(start, end - start));
}

std::vector<std::string> splitCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string::npos) {
      fields.push_back(trim(std::string_view(line).substr(start)));
      break;
    }
    fields.push_back(trim(std::string_view(line).substr(start, comma - start)));
    start = comma + 1;
  }
  return fields;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::string cur;
  // Strip a UTF-8 BOM if present.
  std::size_t pos = 0;
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    pos = 3;
  }
  for (; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else if (c == '\r') {
      // Handled by CRLF's following '\n', or a lone CR line ending.
      if (pos + 1 >= text.size() || text[pos + 1] != '\n') {
        lines.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    lines.push_back(cur);
  }
  return lines;
}

/// ARUS-CSV "ID" column: an optional "0x"/"0X" prefix, then plain hex
/// digits, parsed by the same hex-digit-run parser candump_parser.cpp uses
/// for the frame id/data tokens.
bool parseHexId(const std::string& text, std::uint32_t& value, std::size_t& hex_digit_count) {
  std::string_view s = text;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s = s.substr(2);
  }
  if (!parseHexU32(s, value)) {
    return false;
  }
  hex_digit_count = s.size();
  return true;
}

bool parseSigned(const std::string& text, bool& out) {
  const std::string lowered = PJ::sdk::lowerAscii(text);
  if (lowered == "true" || lowered == "1" || lowered == "yes") {
    out = true;
    return true;
  }
  if (lowered == "false" || lowered == "0" || lowered == "no") {
    out = false;
    return true;
  }
  return false;
}

std::string sanitizeSignalName(const std::string& raw_name, std::map<std::string, int>& used_names) {
  std::string sanitized;
  sanitized.reserve(raw_name.size());
  for (char c : raw_name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    sanitized.push_back(ok ? c : '_');
  }
  if (sanitized.empty()) {
    sanitized = "SIGNAL";
  }
  if (sanitized.front() >= '0' && sanitized.front() <= '9') {
    sanitized = "_" + sanitized;
  }
  const std::string base = sanitized;
  auto it = used_names.find(base);
  if (it == used_names.end()) {
    used_names.emplace(base, 1);
    return base;
  }
  ++it->second;
  return base + "_" + std::to_string(it->second);
}

}  // namespace

std::string formatDbcNumber(double value) {
  std::string text;
  for (int precision : {15, 16, 17}) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::setprecision(precision) << value;
    text = oss.str();
    const auto parsed = PJ::parseNumber<double>(text);
    if (parsed.has_value() && *parsed == value) {
      return text;
    }
  }
  return text;  // precision 17 (the loop's last pass) always round-trips a double
}

PJ::Status arusCsvToDbc(const std::string& csv_text, std::string& out_dbc_text, std::string& warnings) {
  out_dbc_text.clear();
  warnings.clear();

  const std::vector<std::string> lines = splitLines(csv_text);
  if (lines.empty()) {
    return PJ::unexpected(std::string("csv dictionary: empty file"));
  }

  const std::vector<std::string> header = splitCsvLine(lines.front());
  auto findColumn = [&header](const char* name) -> int {
    const std::string target = PJ::sdk::lowerAscii(name);
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (PJ::sdk::lowerAscii(header[i]) == target) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  const int col_id = findColumn("ID");
  const int col_bit_in = findColumn("bitIn");
  const int col_bit_fin = findColumn("bitFin");
  const int col_signed = findColumn("Signed");
  const int col_scale = findColumn("Scale");
  const int col_offset = findColumn("Offset");
  const int col_name = findColumn("Name");
  if (col_id < 0 || col_bit_in < 0 || col_bit_fin < 0 || col_signed < 0 || col_scale < 0 || col_offset < 0 ||
      col_name < 0) {
    return PJ::unexpected(
        std::string("csv dictionary: missing required column (need ID, bitIn, bitFin, Signed, Scale, Offset, Name)"));
  }
  const std::size_t idx_id = static_cast<std::size_t>(col_id);
  const std::size_t idx_bit_in = static_cast<std::size_t>(col_bit_in);
  const std::size_t idx_bit_fin = static_cast<std::size_t>(col_bit_fin);
  const std::size_t idx_signed = static_cast<std::size_t>(col_signed);
  const std::size_t idx_scale = static_cast<std::size_t>(col_scale);
  const std::size_t idx_offset = static_cast<std::size_t>(col_offset);
  const std::size_t idx_name = static_cast<std::size_t>(col_name);
  const std::size_t max_col = std::max({idx_id, idx_bit_in, idx_bit_fin, idx_signed, idx_scale, idx_offset, idx_name});

  // base_id -> its rows, in file order (so message/signal order is stable).
  // A base id above 0x7FF (11-bit standard range) is extended -- derived
  // from the key itself, not stored per-row: every row sharing a base id
  // shares the same extended-ness by construction.
  std::map<std::uint32_t, std::vector<CsvRow>> messages;
  int skipped = 0;

  for (std::size_t line_no = 1; line_no < lines.size(); ++line_no) {
    const std::string& raw_line = lines[line_no];
    if (trim(raw_line).empty()) {
      continue;
    }
    const auto skip = [&](const std::string& reason) {
      warnings += "line " + std::to_string(line_no + 1) + ": " + reason + "\n";
      ++skipped;
    };
    const std::vector<std::string> fields = splitCsvLine(raw_line);
    if (fields.size() <= max_col) {
      skip("too few columns, skipped");
      continue;
    }

    std::uint32_t id_value = 0;
    std::size_t hex_digits = 0;
    if (!parseHexId(fields[idx_id], id_value, hex_digits)) {
      skip("invalid hex ID '" + fields[idx_id] + "', skipped");
      continue;
    }
    // Exactly 4 hex digits: base = first 3, the 4th digit is a per-row
    // signal-index suffix (its value doesn't matter -- rows are grouped by
    // base and re-numbered from bitIn/bitFin, not from this suffix).
    const std::uint32_t base_id = (hex_digits == 4) ? (id_value >> 4) : id_value;

    const auto bit_in_opt = PJ::parseNumber<int>(fields[idx_bit_in]);
    const auto bit_fin_opt = PJ::parseNumber<int>(fields[idx_bit_fin]);
    if (!bit_in_opt || !bit_fin_opt || *bit_in_opt < 0 || *bit_fin_opt > 7 || *bit_in_opt > *bit_fin_opt) {
      skip("invalid bitIn/bitFin, skipped");
      continue;
    }

    bool is_signed = false;
    if (!parseSigned(fields[idx_signed], is_signed)) {
      skip("invalid Signed value, skipped");
      continue;
    }

    const auto scale_opt = PJ::parseNumber<double>(fields[idx_scale]);
    const auto offset_opt = PJ::parseNumber<double>(fields[idx_offset]);
    if (!scale_opt || !offset_opt) {
      skip("invalid Scale/Offset, skipped");
      continue;
    }

    CsvRow row;
    row.bit_in = *bit_in_opt;
    row.bit_fin = *bit_fin_opt;
    row.is_signed = is_signed;
    row.scale = *scale_opt;
    row.offset = *offset_opt;
    row.name = fields[idx_name];

    messages[base_id].push_back(std::move(row));
  }

  if (messages.empty()) {
    return PJ::unexpected(std::string("csv dictionary: no valid rows"));
  }

  std::ostringstream dbc;
  dbc.imbue(std::locale::classic());
  dbc << "VERSION \"\"\n\nNS_ :\n\nBS_:\n\nBU_: PJ\n\n";

  std::map<std::string, int> used_names;
  for (auto& [base_id, rows] : messages) {
    int max_bit_fin = 0;
    for (const auto& row : rows) {
      max_bit_fin = std::max(max_bit_fin, row.bit_fin);
    }
    const int dlc = max_bit_fin + 1;
    const bool extended = base_id > 0x7FFu;
    const std::uint32_t dbc_id = extended ? (base_id | 0x8000'0000u) : base_id;
    // pj_can_dbc::hexId() renders "0xNN"; the message-name convention here
    // has no "0x" prefix, so drop it.
    dbc << "BO_ " << dbc_id << " MSG_" << pj_can_dbc::hexId(base_id).substr(2) << ": " << dlc << " PJ\n";
    for (const auto& row : rows) {
      const std::string signal_name = sanitizeSignalName(row.name, used_names);
      const int start_bit = row.bit_in * 8;
      const int length = (row.bit_fin - row.bit_in + 1) * 8;
      dbc << " SG_ " << signal_name << " : " << start_bit << "|" << length << "@1" << (row.is_signed ? "-" : "+")
          << " (" << formatDbcNumber(row.scale) << "," << formatDbcNumber(row.offset) << ") [0|0] \"\" PJ\n";
    }
    dbc << "\n";
  }

  out_dbc_text = dbc.str();
  if (skipped > 0) {
    warnings = std::to_string(skipped) + " row(s) skipped:\n" + warnings;
  }
  return PJ::okStatus();
}

}  // namespace candump_detail
