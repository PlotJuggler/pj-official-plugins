// SPDX-License-Identifier: MIT
#include "cloud_common.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <locale>
#include <memory>
#include <sstream>

namespace pj3d {

static_assert(
    std::endian::native == std::endian::little,
    "cloud_common assumes a little-endian host; big-endian hosts would silently mislabel is_bigendian=false");

namespace {

uint64_t fieldSize(const ParsedField& f) {
  return static_cast<uint64_t>(PJ::sdk::bytesPerElement(f.datatype)) * static_cast<uint64_t>(f.count);
}

// Write a numeric `value` (given as double) into `dst` at `offset` as datatype
// `dt`, host-endian (little on all supported platforms).
void writeScalar(uint8_t* dst, uint32_t offset, Datatype dt, double value) {
  if (dt != Datatype::kFloat32 && dt != Datatype::kFloat64 && !std::isfinite(value)) {
    // Casting a non-finite/out-of-range double to an integer type is UB.
    value = 0.0;
  }
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
  uint64_t offset = 0;
  for (const auto& f : fields) {
    if (f.datatype == Datatype::kUnknown || PJ::sdk::bytesPerElement(f.datatype) == 0) {
      return PJ::unexpected("field '" + f.name + "' has unknown datatype");
    }
    if (offset > static_cast<uint64_t>(UINT32_MAX)) {
      return PJ::unexpected("point stride exceeds uint32_t range");
    }
    PJ::sdk::PointField pf;
    pf.name = f.name;
    pf.offset = static_cast<uint32_t>(offset);
    pf.datatype = f.datatype;
    pf.count = f.count;
    layout.fields.push_back(std::move(pf));
    offset += fieldSize(f);
  }
  if (offset > static_cast<uint64_t>(UINT32_MAX)) {
    return PJ::unexpected("point stride exceeds uint32_t range");
  }
  layout.point_step = static_cast<uint32_t>(offset);
  return layout;
}

PJ::Expected<PJ::sdk::PointCloud> buildPointCloud(
    const std::vector<ParsedField>& fields, uint32_t width, uint32_t height, bool is_dense, std::string frame_id,
    DataFormat format, PJ::Span<const uint8_t> body) {
  auto layout_or = computeLayout(fields);
  if (!layout_or) {
    return PJ::unexpected(layout_or.error());
  }
  const FieldLayout& layout = *layout_or;
  const uint32_t point_step = layout.point_step;
  const uint64_t num_points = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);

  // A positive point count with a zero stride (no fields) is malformed: it
  // yields an empty payload yet would spin the ascii fill loop num_points
  // times (the binary paths are saved by their body.size() check, ascii is
  // not). Reject it before the loops run.
  if (point_step == 0 && num_points > 0) {
    return PJ::unexpected("point cloud has points but no fields");
  }
  // Reject overflow/oversize BEFORE allocating: point_step*num_points could
  // wrap a narrower type, and even a large-but-non-overflowing size could
  // throw std::bad_alloc/length_error out of resize(). This single check
  // (with point_step != 0 to avoid div-by-zero) catches both cases.
  if (point_step != 0 && num_points > kMaxCloudBytes / static_cast<uint64_t>(point_step)) {
    return PJ::unexpected("point cloud too large");
  }
  const uint64_t out_bytes = num_points * static_cast<uint64_t>(point_step);

  auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(out_bytes));

  if (format == DataFormat::kBinaryLittleEndian) {
    if (body.size() != out_bytes) {
      return PJ::unexpected("binary body size mismatch");
    }
    if (out_bytes > 0) {
      std::memcpy(buf->data(), body.data(), static_cast<size_t>(out_bytes));
    }
  } else if (format == DataFormat::kBinaryBigEndian) {
    if (body.size() != out_bytes) {
      return PJ::unexpected("binary body size mismatch");
    }
    for (uint64_t p = 0; p < num_points; ++p) {
      const uint8_t* src = body.data() + p * point_step;
      uint8_t* dst = buf->data() + p * point_step;
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
    iss.imbue(std::locale::classic());  // dot-decimal parsing regardless of global C++ locale
    for (uint64_t p = 0; p < num_points; ++p) {
      uint8_t* dst = buf->data() + p * point_step;
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

  PJ::sdk::PointCloud cloud;
  cloud.width = width;
  cloud.height = height;
  cloud.point_step = point_step;
  cloud.row_step = static_cast<uint32_t>(static_cast<uint64_t>(point_step) * static_cast<uint64_t>(width));
  cloud.is_bigendian = false;
  cloud.is_dense = is_dense;
  cloud.frame_id = std::move(frame_id);
  cloud.fields = layout.fields;
  cloud.timestamp_ns = 0;
  cloud.data = PJ::Span<const uint8_t>(buf->data(), buf->size());
  cloud.anchor = buf;  // shared_ptr<vector<uint8_t>> -> shared_ptr<const void>; keeps bytes alive
  return cloud;
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
