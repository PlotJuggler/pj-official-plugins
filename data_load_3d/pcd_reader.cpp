// SPDX-License-Identifier: MIT
#include "pcd_reader.hpp"

#include <charconv>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "lzf.h"
}

namespace pj3d {
namespace {

// (TYPE letter, SIZE bytes) -> datatype; kUnknown if unsupported.
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

// Whole token as uint32; nullopt on junk, overflow or trailing characters.
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

// `body` is (uint32 compressed_size, uint32 uncompressed_size, LZF blob) from the DATA line on. The declared
// uncompressed size is validated against the layout BEFORE allocating (decompression-bomb guard); the
// decompressed field-major (SoA) data is transposed to packed points (AoS).
PJ::Expected<PJ::sdk::PointCloud> decodeBinaryCompressed(
    const std::vector<ParsedField>& fields, uint32_t width, uint32_t height, std::string frame_id,
    PJ::Span<const uint8_t> body) {
  constexpr size_t kU32 = 4;
  constexpr size_t kSizeHeaderBytes = 2 * kU32;

  auto layout_or = computeLayout(fields);
  if (!layout_or) {
    return PJ::unexpected(layout_or.error());
  }
  const uint32_t point_step = layout_or->point_step;
  const uint64_t n = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  // Zero stride with points (e.g. all-COUNT-0 fields) is malformed; buildPointCloud rejects it only after the
  // transpose loop would have spun width*height times.
  if (point_step == 0 && n > 0) {
    return PJ::unexpected("PCD binary_compressed: points but zero stride (no fields)");
  }
  // Bound the packed size BEFORE trusting the header.
  if (point_step != 0 && n > kMaxCloudBytes / static_cast<uint64_t>(point_step)) {
    return PJ::unexpected("PCD binary_compressed: cloud too large");
  }
  const uint64_t expected = n * static_cast<uint64_t>(point_step);

  const uint8_t* p = body.data();
  const size_t avail = body.size();
  if (avail < kSizeHeaderBytes) {
    return PJ::unexpected("PCD binary_compressed: truncated size header");
  }
  uint32_t comp_size = 0;
  uint32_t uncomp_size = 0;
  std::memcpy(&comp_size, p, kU32);
  std::memcpy(&uncomp_size, p + kU32, kU32);
  // BEFORE allocating, so a hostile header can't force a huge allocation.
  if (static_cast<uint64_t>(uncomp_size) != expected) {
    return PJ::unexpected("PCD binary_compressed: uncompressed size mismatch");
  }
  if (static_cast<uint64_t>(avail) < static_cast<uint64_t>(comp_size) + kSizeHeaderBytes) {
    return PJ::unexpected("PCD binary_compressed: truncated blob");
  }
  std::vector<uint8_t> soa(static_cast<size_t>(uncomp_size));
  const unsigned int got = lzf_decompress(p + kSizeHeaderBytes, comp_size, soa.data(), uncomp_size);
  if (got != uncomp_size) {
    return PJ::unexpected("PCD binary_compressed: LZF decompress failed");
  }
  std::vector<uint8_t> aos(static_cast<size_t>(expected));
  size_t soa_off = 0;
  for (const auto& pf : layout_or->fields) {
    const uint64_t fsize =
        static_cast<uint64_t>(PJ::sdk::bytesPerElement(pf.datatype)) * static_cast<uint64_t>(pf.count);
    for (uint64_t pt = 0; pt < n; ++pt) {
      std::memcpy(
          aos.data() + pt * static_cast<uint64_t>(point_step) + pf.offset, soa.data() + soa_off,
          static_cast<size_t>(fsize));
      soa_off += static_cast<size_t>(fsize);
    }
  }
  return buildPointCloud(
      fields, width, height, /*is_dense=*/true, std::move(frame_id), DataFormat::kBinaryLittleEndian,
      PJ::Span<const uint8_t>(aos.data(), aos.size()));
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
  if (data_mode != "ascii" && data_mode != "binary" && data_mode != "binary_compressed") {
    return PJ::unexpected("PCD DATA mode not recognized: " + data_mode);
  }

  // COUNT is optional (default 1 per field); if present it needs one entry per field, like SIZE/TYPE.
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

  if (data_mode == "binary_compressed") {
    PJ::Span<const uint8_t> body(file_bytes.data() + body_offset, total - body_offset);
    return decodeBinaryCompressed(fields, width, height, std::move(frame_id), body);
  }

  const DataFormat fmt = (data_mode == "ascii") ? DataFormat::kAscii : DataFormat::kBinaryLittleEndian;
  PJ::Span<const uint8_t> body(file_bytes.data() + body_offset, total - body_offset);
  return buildPointCloud(fields, width, height, /*is_dense=*/true, std::move(frame_id), fmt, body);
}

}  // namespace pj3d
