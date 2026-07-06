// SPDX-License-Identifier: MIT
#include "pcd_reader.hpp"

#include <charconv>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pj3d {
namespace {

// Map (TYPE letter, SIZE bytes) to a canonical datatype. kUnknown if unsupported.
Datatype toDatatype(char type, uint32_t size) {
  switch (type) {
    case 'I':
      if (size == 1) {
        return Datatype::kInt8;
      }
      if (size == 2) {
        return Datatype::kInt16;
      }
      if (size == 4) {
        return Datatype::kInt32;
      }
      return Datatype::kUnknown;
    case 'U':
      if (size == 1) {
        return Datatype::kUint8;
      }
      if (size == 2) {
        return Datatype::kUint16;
      }
      if (size == 4) {
        return Datatype::kUint32;
      }
      return Datatype::kUnknown;
    case 'F':
      if (size == 4) {
        return Datatype::kFloat32;
      }
      if (size == 8) {
        return Datatype::kFloat64;
      }
      return Datatype::kUnknown;
    default:
      return Datatype::kUnknown;
  }
}

// Parse a whole token as an unsigned 32-bit int. nullopt on any junk / overflow
// / trailing characters, so callers can surface a clean error instead of throwing.
std::optional<uint32_t> parseU32(const std::string& tok) {
  uint32_t value = 0;
  const char* first = tok.data();
  const char* last = tok.data() + tok.size();
  auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last) {
    return std::nullopt;
  }
  return value;
}

std::vector<std::string> tokenize(const std::string& line) {
  std::istringstream iss(line);
  std::vector<std::string> out;
  std::string tok;
  while (iss >> tok) {
    out.push_back(tok);
  }
  return out;
}

}  // namespace

PJ::Expected<PJ::sdk::PointCloud> readPcd(PJ::Span<const uint8_t> file_bytes, std::string frame_id) {
  const char* begin = reinterpret_cast<const char*>(file_bytes.data());
  const size_t total = file_bytes.size();

  std::vector<std::string> names;
  std::vector<std::string> types_tok;
  std::vector<std::string> sizes_tok;
  std::vector<std::string> counts_tok;
  uint32_t width = 0;
  uint32_t height = 1;
  bool have_width = false;
  bool have_height = false;
  std::string data_mode;
  size_t body_offset = 0;

  size_t pos = 0;
  while (pos < total) {
    size_t nl = pos;
    while (nl < total && begin[nl] != '\n') {
      ++nl;
    }
    std::string line(begin + pos, nl - pos);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    pos = (nl < total) ? nl + 1 : total;

    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto tok = tokenize(line);
    if (tok.empty()) {
      continue;
    }
    const std::string& key = tok[0];
    if (key == "FIELDS") {
      names.assign(tok.begin() + 1, tok.end());
    } else if (key == "SIZE") {
      sizes_tok.assign(tok.begin() + 1, tok.end());
    } else if (key == "TYPE") {
      types_tok.assign(tok.begin() + 1, tok.end());
    } else if (key == "COUNT") {
      counts_tok.assign(tok.begin() + 1, tok.end());
    } else if (key == "WIDTH" && tok.size() >= 2) {
      auto w = parseU32(tok[1]);
      if (!w) {
        return PJ::unexpected("PCD header: WIDTH is not a valid number");
      }
      width = *w;
      have_width = true;
    } else if (key == "HEIGHT" && tok.size() >= 2) {
      auto h = parseU32(tok[1]);
      if (!h) {
        return PJ::unexpected("PCD header: HEIGHT is not a valid number");
      }
      height = *h;
      have_height = true;
    } else if (key == "DATA" && tok.size() >= 2) {
      data_mode = tok[1];
      body_offset = pos;
      break;
    }
  }

  if (names.empty() || sizes_tok.size() != names.size() || types_tok.size() != names.size()) {
    return PJ::unexpected("PCD header: FIELDS/SIZE/TYPE missing or mismatched");
  }
  if (!have_width || !have_height) {
    return PJ::unexpected("PCD header: WIDTH/HEIGHT missing");
  }
  if (data_mode == "binary_compressed") {
    return PJ::unexpected("PCD DATA binary_compressed is not yet supported");
  }
  if (data_mode != "ascii" && data_mode != "binary") {
    return PJ::unexpected("PCD DATA mode not recognized: " + data_mode);
  }

  // COUNT is optional: absent entirely -> default 1 per field. But if present,
  // it must have one entry per field (same arity rule as SIZE/TYPE).
  const bool have_counts = !counts_tok.empty();
  if (have_counts && counts_tok.size() != names.size()) {
    return PJ::unexpected("PCD header: COUNT count does not match FIELDS");
  }

  std::vector<ParsedField> fields;
  fields.reserve(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    auto size = parseU32(sizes_tok[i]);
    if (!size) {
      return PJ::unexpected("PCD field '" + names[i] + "': SIZE is not a valid number");
    }
    uint32_t count = 1;
    if (have_counts) {
      auto c = parseU32(counts_tok[i]);
      if (!c) {
        return PJ::unexpected("PCD field '" + names[i] + "': COUNT is not a valid number");
      }
      count = *c;
    }
    if (types_tok[i].size() != 1) {
      return PJ::unexpected(
          "PCD field '" + names[i] + "': TYPE must be a single letter (I/U/F), got '" + types_tok[i] + "'");
    }
    const Datatype dt = toDatatype(types_tok[i][0], *size);
    if (dt == Datatype::kUnknown) {
      return PJ::unexpected("PCD field '" + names[i] + "': unsupported TYPE/SIZE " + types_tok[i] + "/" + sizes_tok[i]);
    }
    fields.push_back(ParsedField{names[i], dt, count});
  }

  const DataFormat fmt = (data_mode == "ascii") ? DataFormat::kAscii : DataFormat::kBinaryLittleEndian;
  PJ::Span<const uint8_t> body(file_bytes.data() + body_offset, total - body_offset);
  return buildPointCloud(fields, width, height, /*is_dense=*/true, std::move(frame_id), fmt, body);
}

}  // namespace pj3d
