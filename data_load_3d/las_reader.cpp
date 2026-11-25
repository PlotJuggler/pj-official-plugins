// SPDX-License-Identifier: MIT
#include "las_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

#include "cloud_common.hpp"

namespace pj3d {

namespace {

// Public header block offsets (ASPRS LAS 1.4 R15, table 3; 1.0-1.3 share the prefix).
constexpr size_t kOffVersionMajor = 24;
constexpr size_t kOffVersionMinor = 25;
constexpr size_t kOffHeaderSize = 94;
constexpr size_t kOffPointDataOffset = 96;
constexpr size_t kOffPointFormat = 104;
constexpr size_t kOffRecordLength = 105;
constexpr size_t kOffLegacyPointCount = 107;
constexpr size_t kOffScale = 131;
constexpr size_t kOffOffset = 155;
constexpr size_t kOffPointCount64 = 247;  // LAS 1.4 only
constexpr size_t kMinHeaderSize = 227;    // LAS 1.0-1.2
constexpr size_t kHeaderSize14 = 375;

// An axis whose coordinates exceed this magnitude (in file units) is re-centred
// before narrowing to float32: at 1e4 a float32 step is ~1 mm, while georeferenced
// LAS coordinates (UTM, state plane) reach 1e6-1e7, where it is 0.1-1 m.
constexpr double kLasShiftThreshold = 1e4;

// Point format id bits 7/6 mark LASzip-compressed records (laszip's convention).
constexpr uint8_t kLazBits = 0xC0;
constexpr uint8_t kMaxPointFormat = 10;

// Minimum record length per point data record format 0-10.
constexpr std::array<uint16_t, 11> kMinRecordLength = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};

template <typename T>
T readLE(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));  // host is little-endian (static_assert in cloud_common.cpp)
  return v;
}

bool hasGpsTime(uint8_t fmt) {
  return fmt != 0 && fmt != 2;
}

bool hasRgb(uint8_t fmt) {
  return fmt == 2 || fmt == 3 || fmt == 5 || fmt == 7 || fmt == 8 || fmt == 10;
}

bool hasNir(uint8_t fmt) {
  return fmt == 8 || fmt == 10;
}

// Byte offset of GPS time / RGB within a source record of format `fmt`.
size_t gpsOffset(uint8_t fmt) {
  return fmt >= 6 ? 22 : 20;
}

size_t rgbOffset(uint8_t fmt) {
  switch (fmt) {
    case 2:
      return 20;
    case 3:
    case 5:
      return 28;
    default:  // 7, 8, 10; not read for formats without RGB
      return 30;
  }
}

const PJ::sdk::PointField& field(const FieldLayout& layout, const char* name) {
  return *std::find_if(
      layout.fields.begin(), layout.fields.end(), [name](const PJ::sdk::PointField& f) { return f.name == name; });
}

template <typename T>
void put(uint8_t* dst, uint32_t offset, T v) {
  std::memcpy(dst + offset, &v, sizeof(T));
}

}  // namespace

PJ::Expected<LasResult> readLas(PJ::Span<const uint8_t> file_bytes, std::string frame_id) {
  const uint8_t* file = file_bytes.data();
  const size_t file_size = file_bytes.size();
  if (file_size < kMinHeaderSize || std::memcmp(file, "LASF", 4) != 0) {
    return PJ::unexpected(std::string("not a LAS file (missing LASF signature or truncated header)"));
  }

  LasResult result;
  const uint8_t version_major = file[kOffVersionMajor];
  const uint8_t version_minor = file[kOffVersionMinor];
  if (version_major != 1 || version_minor > 4) {
    return PJ::unexpected(
        "unsupported LAS version " + std::to_string(version_major) + "." + std::to_string(version_minor));
  }

  const size_t header_size = readLE<uint16_t>(file + kOffHeaderSize);
  const uint64_t data_offset = readLE<uint32_t>(file + kOffPointDataOffset);
  const uint8_t fmt = file[kOffPointFormat];
  const uint16_t record_length = readLE<uint16_t>(file + kOffRecordLength);

  if ((fmt & kLazBits) != 0) {
    return PJ::unexpected(std::string("LAZ-compressed point data is not supported; decompress to .las first"));
  }
  if (fmt > kMaxPointFormat) {
    return PJ::unexpected("unsupported LAS point data record format " + std::to_string(fmt));
  }
  if (record_length < kMinRecordLength[fmt]) {
    return PJ::unexpected(
        "LAS point record length " + std::to_string(record_length) + " is shorter than format " + std::to_string(fmt) +
        " requires (" + std::to_string(kMinRecordLength[fmt]) + ")");
  }
  if (header_size < kMinHeaderSize || header_size > file_size || data_offset < header_size || data_offset > file_size) {
    return PJ::unexpected(std::string("malformed LAS header (header size or point data offset out of range)"));
  }

  // Some writers bump a 1.2 header to 1.4 and leave the 64-bit count at 0.
  uint64_t num_points = readLE<uint32_t>(file + kOffLegacyPointCount);
  if (version_minor >= 4 && header_size >= kHeaderSize14) {
    if (const uint64_t count64 = readLE<uint64_t>(file + kOffPointCount64); count64 != 0) {
      num_points = count64;
    }
  }

  std::array<double, 3> scale{};
  std::array<double, 3> offset{};
  for (size_t a = 0; a < 3; ++a) {
    scale[a] = readLE<double>(file + kOffScale + 8 * a);
    offset[a] = readLE<double>(file + kOffOffset + 8 * a);
    if (!std::isfinite(scale[a]) || scale[a] == 0.0 || !std::isfinite(offset[a])) {
      return PJ::unexpected(std::string("malformed LAS header (non-finite or zero scale/offset)"));
    }
  }
  const auto world = [&](int32_t raw, size_t a) { return static_cast<double>(raw) * scale[a] + offset[a]; };

  const uint64_t available = file_size - data_offset;
  if (num_points > available / record_length) {
    return PJ::unexpected(
        "LAS file truncated: header declares " + std::to_string(num_points) + " points, data holds " +
        std::to_string(available / record_length));
  }
  const uint8_t* records = file + data_offset;

  const bool has_gps = hasGpsTime(fmt);
  const bool has_rgb = hasRgb(fmt);
  const bool has_nir = hasNir(fmt);
  const size_t gps_src = gpsOffset(fmt);
  const size_t rgb_src = rgbOffset(fmt);

  std::vector<ParsedField> parsed = {
      {"x", Datatype::kFloat32, 1},
      {"y", Datatype::kFloat32, 1},
      {"z", Datatype::kFloat32, 1},
      {"intensity", Datatype::kUint16, 1},
      {"return_number", Datatype::kUint8, 1},
      {"number_of_returns", Datatype::kUint8, 1},
      {"classification", Datatype::kUint8, 1},
      {"classification_flags", Datatype::kUint8, 1},
      {"scan_angle", Datatype::kFloat32, 1},
      {"user_data", Datatype::kUint8, 1},
      {"point_source_id", Datatype::kUint16, 1},
  };
  if (has_gps) {
    parsed.push_back({"gps_time", Datatype::kFloat64, 1});
  }
  if (has_rgb) {
    parsed.push_back({"red", Datatype::kUint8, 1});
    parsed.push_back({"green", Datatype::kUint8, 1});
    parsed.push_back({"blue", Datatype::kUint8, 1});
  }
  if (has_nir) {
    parsed.push_back({"nir", Datatype::kUint16, 1});
  }
  auto layout_or = computeLayout(parsed);
  if (!layout_or) {
    return PJ::unexpected(layout_or.error());
  }
  const FieldLayout& layout = *layout_or;
  const uint32_t step = layout.point_step;
  // buildPointCloud applies the same cap; checking here too keeps the staging
  // buffer below from being sized by an unchecked header count. It also bounds
  // num_points below 2^32, so the width cast is exact.
  if (num_points > kMaxCloudBytes / step) {
    return PJ::unexpected(std::string("point cloud too large"));
  }

  // First pass: raw coordinate range (for the float32 re-centring) and the RGB depth.
  std::array<int32_t, 3> raw_lo;
  std::array<int32_t, 3> raw_hi;
  raw_lo.fill(std::numeric_limits<int32_t>::max());
  raw_hi.fill(std::numeric_limits<int32_t>::min());
  uint16_t max_rgb = 0;
  for (uint64_t p = 0; p < num_points; ++p) {
    const uint8_t* rec = records + p * record_length;
    for (size_t a = 0; a < 3; ++a) {
      const int32_t v = readLE<int32_t>(rec + 4 * a);
      raw_lo[a] = std::min(raw_lo[a], v);
      raw_hi[a] = std::max(raw_hi[a], v);
    }
    if (has_rgb) {
      const uint8_t* rgb = rec + rgb_src;
      max_rgb = std::max({max_rgb, readLE<uint16_t>(rgb), readLE<uint16_t>(rgb + 2), readLE<uint16_t>(rgb + 4)});
    }
  }
  for (size_t a = 0; a < 3 && num_points > 0; ++a) {
    const double lo = std::min(world(raw_lo[a], a), world(raw_hi[a], a));  // a negative scale swaps the ends
    const double hi = std::max(world(raw_lo[a], a), world(raw_hi[a], a));
    if (std::max(std::abs(lo), std::abs(hi)) > kLasShiftThreshold) {
      // The midpoint, not the minimum: an axis straddling zero would otherwise double its largest magnitude.
      result.origin[a] = std::floor(std::midpoint(lo, hi));
    }
    const double reach = std::max(std::abs(lo - result.origin[a]), std::abs(hi - result.origin[a]));
    if (!std::isfinite(lo) || !std::isfinite(hi) || reach > std::numeric_limits<float>::max()) {
      return PJ::unexpected(std::string("malformed LAS header (scale/offset put coordinates outside float32)"));
    }
  }
  // The spec stores 16-bit colour, but many writers store 8-bit values in those
  // fields; any value above 255 means the file uses the full 16-bit range.
  const int rgb_shift = max_rgb > 255 ? 8 : 0;

  const uint32_t off_x = field(layout, "x").offset;
  const uint32_t off_intensity = field(layout, "intensity").offset;
  const uint32_t off_return = field(layout, "return_number").offset;
  const uint32_t off_returns = field(layout, "number_of_returns").offset;
  const uint32_t off_class = field(layout, "classification").offset;
  const uint32_t off_flags = field(layout, "classification_flags").offset;
  const uint32_t off_angle = field(layout, "scan_angle").offset;
  const uint32_t off_user = field(layout, "user_data").offset;
  const uint32_t off_source = field(layout, "point_source_id").offset;
  const uint32_t off_gps = has_gps ? field(layout, "gps_time").offset : 0;
  const uint32_t off_red = has_rgb ? field(layout, "red").offset : 0;
  const uint32_t off_nir = has_nir ? field(layout, "nir").offset : 0;

  std::vector<uint8_t> body(static_cast<size_t>(num_points * step));
  for (uint64_t p = 0; p < num_points; ++p) {
    const uint8_t* rec = records + p * record_length;
    uint8_t* dst = body.data() + p * step;
    for (uint32_t a = 0; a < 3; ++a) {
      put(dst, off_x + 4 * a, static_cast<float>(world(readLE<int32_t>(rec + 4 * a), a) - result.origin[a]));
    }
    put(dst, off_intensity, readLE<uint16_t>(rec + 12));
    put(dst, off_user, rec[17]);
    const uint8_t returns_byte = rec[14];
    if (fmt < 6) {
      // Byte 14: return number bits 0-2, number of returns bits 3-5. Byte 15:
      // class bits 0-4, synthetic/key-point/withheld bits 5-7.
      put(dst, off_return, static_cast<uint8_t>(returns_byte & 0x07));
      put(dst, off_returns, static_cast<uint8_t>((returns_byte >> 3) & 0x07));
      put(dst, off_class, static_cast<uint8_t>(rec[15] & 0x1F));
      put(dst, off_flags, static_cast<uint8_t>(rec[15] >> 5));
      put(dst, off_angle, static_cast<float>(readLE<int8_t>(rec + 16)));
      put(dst, off_source, readLE<uint16_t>(rec + 18));
    } else {
      // Byte 14: return number bits 0-3, number of returns bits 4-7. Byte 15:
      // classification flags bits 0-3. Scan angle in 0.006 degree units.
      put(dst, off_return, static_cast<uint8_t>(returns_byte & 0x0F));
      put(dst, off_returns, static_cast<uint8_t>(returns_byte >> 4));
      put(dst, off_class, rec[16]);
      put(dst, off_flags, static_cast<uint8_t>(rec[15] & 0x0F));
      put(dst, off_angle, static_cast<float>(readLE<int16_t>(rec + 18)) * 0.006f);
      put(dst, off_source, readLE<uint16_t>(rec + 20));
    }
    if (has_gps) {
      put(dst, off_gps, readLE<double>(rec + gps_src));
    }
    if (has_rgb) {
      const uint8_t* rgb = rec + rgb_src;
      for (uint32_t c = 0; c < 3; ++c) {
        put(dst, off_red + c, static_cast<uint8_t>(readLE<uint16_t>(rgb + 2 * c) >> rgb_shift));
      }
      if (has_nir) {
        put(dst, off_nir, readLE<uint16_t>(rgb + 6));
      }
    }
  }

  auto cloud = buildPointCloud(
      parsed, static_cast<uint32_t>(num_points), /*height=*/1, /*is_dense=*/true, std::move(frame_id),
      DataFormat::kBinaryLittleEndian, PJ::Span<const uint8_t>(body.data(), body.size()));
  if (!cloud) {
    return PJ::unexpected(cloud.error());
  }
  result.cloud = std::move(*cloud);
  return result;
}

}  // namespace pj3d
