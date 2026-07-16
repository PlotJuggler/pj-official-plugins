// SPDX-License-Identifier: MIT
#include "ply_reader.hpp"

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pj3d {
namespace {

Datatype plyType(const std::string& t) {
  if (t == "char" || t == "int8") {
    return Datatype::kInt8;
  }
  if (t == "uchar" || t == "uint8") {
    return Datatype::kUint8;
  }
  if (t == "short" || t == "int16") {
    return Datatype::kInt16;
  }
  if (t == "ushort" || t == "uint16") {
    return Datatype::kUint16;
  }
  if (t == "int" || t == "int32") {
    return Datatype::kInt32;
  }
  if (t == "uint" || t == "uint32") {
    return Datatype::kUint32;
  }
  if (t == "float" || t == "float32") {
    return Datatype::kFloat32;
  }
  if (t == "double" || t == "float64") {
    return Datatype::kFloat64;
  }
  return Datatype::kUnknown;
}

// Parse a whole token as an unsigned 64-bit int. nullopt on any junk / overflow
// / trailing characters, so callers can surface a clean error instead of throwing.
std::optional<uint64_t> parseU64(const std::string& tok) {
  uint64_t value = 0;
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

PJ::Expected<PlyResult> readPly(PJ::Span<const uint8_t> file_bytes, std::string frame_id) {
  const char* begin = reinterpret_cast<const char*>(file_bytes.data());
  const size_t total = file_bytes.size();

  std::string format_str;
  uint64_t vertex_count = 0;
  uint64_t face_count = 0;
  std::vector<std::string> vertex_prop_types;  // parallel arrays, source order
  std::vector<std::string> vertex_prop_names;  //
  bool vertex_has_unsupported_prop = false;    // list property or malformed (<3-token) property line
  bool in_vertex = false;
  size_t body_offset = 0;
  bool header_done = false;
  bool first_content_line = true;

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

    auto tok = tokenize(line);
    if (tok.empty()) {
      continue;
    }
    if (first_content_line) {
      first_content_line = false;
      if (tok[0] != "ply") {
        return PJ::unexpected("PLY: missing 'ply' magic on the first line");
      }
      continue;  // the magic line carries nothing else
    }
    if (tok[0] == "format" && tok.size() >= 2) {
      format_str = tok[1];
    } else if (tok[0] == "element") {
      if (tok.size() < 3) {
        return PJ::unexpected("PLY: malformed element line");
      }
      if (tok[1] == "vertex") {
        auto vc = parseU64(tok[2]);
        if (!vc) {
          return PJ::unexpected("PLY: invalid vertex count '" + tok[2] + "'");
        }
        vertex_count = *vc;
        in_vertex = true;
      } else {
        if (tok[1] == "face") {
          auto fc = parseU64(tok[2]);
          if (!fc) {
            return PJ::unexpected("PLY: invalid face count '" + tok[2] + "'");
          }
          face_count = *fc;
        }
        in_vertex = false;
      }
    } else if (tok[0] == "property" && in_vertex) {
      // Record only; validation is deferred to the cloud path. A file with
      // faces ignores vertex properties entirely (raw bytes go to the renderer).
      if (tok.size() >= 2 && tok[1] == "list") {
        vertex_has_unsupported_prop = true;
      } else if (tok.size() >= 3) {
        vertex_prop_types.push_back(tok[1]);
        vertex_prop_names.push_back(tok[2]);
      } else {
        vertex_has_unsupported_prop = true;  // malformed "property" line
      }
    } else if (tok[0] == "end_header") {
      body_offset = pos;
      header_done = true;
      break;
    }
  }

  if (!header_done) {
    return PJ::unexpected("PLY: missing end_header");
  }

  PlyResult result;
  result.num_faces = face_count;

  // Faces present -> hand the raw file bytes to the renderer as a self-owning Mesh3D.
  if (face_count > 0) {
    auto buf = std::make_shared<std::vector<uint8_t>>(file_bytes.data(), file_bytes.data() + total);
    PJ::sdk::Mesh3D mesh;
    mesh.timestamp_ns = 0;
    mesh.frame_id = std::move(frame_id);
    mesh.format = "ply";
    mesh.data = PJ::Span<const uint8_t>(buf->data(), buf->size());
    mesh.anchor = buf;  // self-owning: bytes outlive `file_bytes`
    result.mesh = std::move(mesh);
    return result;
  }

  // Vertices only -> point cloud. Validate the deferred vertex properties now.
  if (vertex_has_unsupported_prop) {
    return PJ::unexpected("PLY: vertex element has an unsupported (list or malformed) property");
  }
  if (vertex_prop_names.empty()) {
    return PJ::unexpected("PLY: no vertex properties found");
  }
  std::vector<ParsedField> vertex_fields;
  vertex_fields.reserve(vertex_prop_names.size());
  for (size_t i = 0; i < vertex_prop_names.size(); ++i) {
    const Datatype dt = plyType(vertex_prop_types[i]);
    if (dt == Datatype::kUnknown) {
      return PJ::unexpected(
          "PLY vertex property '" + vertex_prop_names[i] + "': unsupported type " + vertex_prop_types[i]);
    }
    vertex_fields.push_back(ParsedField{vertex_prop_names[i], dt, 1});
  }
  if (vertex_count > static_cast<uint64_t>(UINT32_MAX)) {
    return PJ::unexpected("PLY: vertex count exceeds supported maximum");
  }
  DataFormat fmt;
  if (format_str == "ascii") {
    fmt = DataFormat::kAscii;
  } else if (format_str == "binary_little_endian") {
    fmt = DataFormat::kBinaryLittleEndian;
  } else if (format_str == "binary_big_endian") {
    fmt = DataFormat::kBinaryBigEndian;
  } else {
    return PJ::unexpected("PLY: unknown format '" + format_str + "'");
  }

  PJ::Span<const uint8_t> body(file_bytes.data() + body_offset, total - body_offset);
  auto built = buildPointCloud(
      vertex_fields, static_cast<uint32_t>(vertex_count), 1,
      /*is_dense=*/true, std::move(frame_id), fmt, body);
  if (!built) {
    return PJ::unexpected(built.error());
  }
  result.cloud = std::move(*built);
  return result;
}

}  // namespace pj3d
