// SPDX-License-Identifier: MIT
#include "cloud_common.hpp"

#include <array>
#include <cstring>
#include <sstream>

namespace pj3d {
namespace {

uint32_t fieldSize(const ParsedField& f) {
  return PJ::sdk::bytesPerElement(f.datatype) * f.count;
}

// Write a numeric `value` (given as double) into `dst` at `offset` as datatype
// `dt`, host-endian (little on all supported platforms).
void writeScalar(uint8_t* dst, uint32_t offset, Datatype dt, double value) {
  switch (dt) {
    case Datatype::kInt8: {
      auto v = static_cast<int8_t>(value);
      std::memcpy(dst + offset, &v, 1);
      break;
    }
    case Datatype::kUint8: {
      auto v = static_cast<uint8_t>(value);
      std::memcpy(dst + offset, &v, 1);
      break;
    }
    case Datatype::kInt16: {
      auto v = static_cast<int16_t>(value);
      std::memcpy(dst + offset, &v, 2);
      break;
    }
    case Datatype::kUint16: {
      auto v = static_cast<uint16_t>(value);
      std::memcpy(dst + offset, &v, 2);
      break;
    }
    case Datatype::kInt32: {
      auto v = static_cast<int32_t>(value);
      std::memcpy(dst + offset, &v, 4);
      break;
    }
    case Datatype::kUint32: {
      auto v = static_cast<uint32_t>(value);
      std::memcpy(dst + offset, &v, 4);
      break;
    }
    case Datatype::kFloat32: {
      auto v = static_cast<float>(value);
      std::memcpy(dst + offset, &v, 4);
      break;
    }
    case Datatype::kFloat64: {
      std::memcpy(dst + offset, &value, 8);
      break;
    }
    case Datatype::kUnknown:
      break;
  }
}

// Read a scalar as double from little-endian packed bytes.
double readScalar(const uint8_t* src, Datatype dt) {
  switch (dt) {
    case Datatype::kInt8: {
      int8_t v;
      std::memcpy(&v, src, 1);
      return static_cast<double>(v);
    }
    case Datatype::kUint8: {
      uint8_t v;
      std::memcpy(&v, src, 1);
      return static_cast<double>(v);
    }
    case Datatype::kInt16: {
      int16_t v;
      std::memcpy(&v, src, 2);
      return static_cast<double>(v);
    }
    case Datatype::kUint16: {
      uint16_t v;
      std::memcpy(&v, src, 2);
      return static_cast<double>(v);
    }
    case Datatype::kInt32: {
      int32_t v;
      std::memcpy(&v, src, 4);
      return static_cast<double>(v);
    }
    case Datatype::kUint32: {
      uint32_t v;
      std::memcpy(&v, src, 4);
      return static_cast<double>(v);
    }
    case Datatype::kFloat32: {
      float v;
      std::memcpy(&v, src, 4);
      return static_cast<double>(v);
    }
    case Datatype::kFloat64: {
      double v;
      std::memcpy(&v, src, 8);
      return v;
    }
    case Datatype::kUnknown:
      return 0.0;
  }
  return 0.0;
}

}  // namespace

PJ::Expected<FieldLayout> computeLayout(const std::vector<ParsedField>& fields) {
  FieldLayout layout;
  layout.fields.reserve(fields.size());
  uint32_t offset = 0;
  for (const auto& f : fields) {
    if (f.datatype == Datatype::kUnknown || PJ::sdk::bytesPerElement(f.datatype) == 0) {
      return PJ::unexpected("field '" + f.name + "' has unknown datatype");
    }
    PJ::sdk::PointField pf;
    pf.name = f.name;
    pf.offset = offset;
    pf.datatype = f.datatype;
    pf.count = f.count;
    layout.fields.push_back(std::move(pf));
    offset += fieldSize(f);
  }
  layout.point_step = offset;
  return layout;
}

PJ::Expected<BuiltCloud> buildPointCloud(
    const std::vector<ParsedField>& fields, uint32_t width, uint32_t height, bool is_dense, std::string frame_id,
    DataFormat format, PJ::Span<const uint8_t> body) {
  auto layout_or = computeLayout(fields);
  if (!layout_or) {
    return PJ::unexpected(layout_or.error());
  }
  const FieldLayout& layout = *layout_or;
  const uint32_t point_step = layout.point_step;
  const uint64_t num_points = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  const uint64_t out_bytes = num_points * point_step;

  BuiltCloud out;
  out.storage.resize(static_cast<size_t>(out_bytes));

  if (format == DataFormat::kBinaryLittleEndian) {
    if (body.size() != out_bytes) {
      return PJ::unexpected("binary body size mismatch");
    }
    if (out_bytes > 0) {
      std::memcpy(out.storage.data(), body.data(), static_cast<size_t>(out_bytes));
    }
  } else if (format == DataFormat::kBinaryBigEndian) {
    if (body.size() != out_bytes) {
      return PJ::unexpected("binary body size mismatch");
    }
    for (uint64_t p = 0; p < num_points; ++p) {
      const uint8_t* src = body.data() + p * point_step;
      uint8_t* dst = out.storage.data() + p * point_step;
      for (const auto& pf : layout.fields) {
        const uint32_t elem = PJ::sdk::bytesPerElement(pf.datatype);
        for (uint32_t e = 0; e < pf.count; ++e) {
          const uint32_t off = pf.offset + e * elem;
          for (uint32_t b = 0; b < elem; ++b) {
            dst[off + b] = src[off + elem - 1 - b];
          }
        }
      }
    }
  } else {  // kAscii
    std::string text(reinterpret_cast<const char*>(body.data()), body.size());
    std::istringstream iss(text);
    for (uint64_t p = 0; p < num_points; ++p) {
      uint8_t* dst = out.storage.data() + p * point_step;
      for (const auto& pf : layout.fields) {
        const uint32_t elem = PJ::sdk::bytesPerElement(pf.datatype);
        for (uint32_t e = 0; e < pf.count; ++e) {
          double value = 0.0;
          if (!(iss >> value)) {
            return PJ::unexpected("ascii body: not enough numeric tokens");
          }
          writeScalar(dst, pf.offset + e * elem, pf.datatype, value);
        }
      }
    }
  }

  out.cloud.width = width;
  out.cloud.height = height;
  out.cloud.point_step = point_step;
  out.cloud.row_step = point_step * width;
  out.cloud.is_bigendian = false;
  out.cloud.is_dense = is_dense;
  out.cloud.frame_id = std::move(frame_id);
  out.cloud.fields = layout.fields;
  out.cloud.timestamp_ns = 0;
  out.cloud.data = PJ::Span<const uint8_t>(out.storage.data(), out.storage.size());
  return out;
}

std::optional<std::array<double, 3>> computeCentroid(const PJ::sdk::PointCloud& cloud) {
  const PJ::sdk::PointField* fx = nullptr;
  const PJ::sdk::PointField* fy = nullptr;
  const PJ::sdk::PointField* fz = nullptr;
  for (const auto& f : cloud.fields) {
    if (f.name == "x") {
      fx = &f;
    } else if (f.name == "y") {
      fy = &f;
    } else if (f.name == "z") {
      fz = &f;
    }
  }
  if (fx == nullptr || fy == nullptr || fz == nullptr) {
    return std::nullopt;
  }
  const uint64_t n = static_cast<uint64_t>(cloud.width) * static_cast<uint64_t>(cloud.height);
  if (n == 0) {
    return std::nullopt;
  }
  std::array<double, 3> sum{0.0, 0.0, 0.0};
  for (uint64_t p = 0; p < n; ++p) {
    const uint8_t* base = cloud.data.data() + p * cloud.point_step;
    sum[0] += readScalar(base + fx->offset, fx->datatype);
    sum[1] += readScalar(base + fy->offset, fy->datatype);
    sum[2] += readScalar(base + fz->offset, fz->datatype);
  }
  const double dn = static_cast<double>(n);
  return std::array<double, 3>{sum[0] / dn, sum[1] / dn, sum[2] / dn};
}

}  // namespace pj3d
