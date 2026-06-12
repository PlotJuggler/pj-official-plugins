// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include "foxglove_pointcloud_codec.hpp"

#include <google/protobuf/io/coded_stream.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <string>
#include <vector>

namespace pj_protobuf {
namespace {

namespace gpio = google::protobuf::io;

using Datatype = PJ::sdk::PointField::Datatype;

// Field numbers of foxglove.PointCloud / PackedElementField (see header).
constexpr int kFieldTimestamp = 1;
constexpr int kFieldFrameId = 2;
constexpr int kFieldPose = 3;
constexpr int kFieldPointStride = 4;
constexpr int kFieldFields = 5;
constexpr int kFieldData = 6;

constexpr int kPefName = 1;
constexpr int kPefOffset = 2;
constexpr int kPefType = 3;

// Field numbers of foxglove.LaserScan (see header).
constexpr int kLsTimestamp = 1;
constexpr int kLsFrameId = 2;
constexpr int kLsPose = 3;
constexpr int kLsStartAngle = 4;
constexpr int kLsEndAngle = 5;
constexpr int kLsRanges = 6;
constexpr int kLsIntensities = 7;

// Protobuf wire types.
constexpr uint32_t kWireVarint = 0;
constexpr uint32_t kWireI64 = 1;
constexpr uint32_t kWireLen = 2;
constexpr uint32_t kWireI32 = 5;

/// Foxglove NumericType -> canonical PJ datatype. The enum swaps signed and
/// unsigned variants relative to ROS/SDK (UINT8=1 here vs INT8=1 there), so the
/// ROS mapper cannot be reused.
[[nodiscard]] Datatype mapFoxgloveNumericType(uint64_t t) {
  switch (t) {
    case 1:
      return Datatype::kUint8;
    case 2:
      return Datatype::kInt8;
    case 3:
      return Datatype::kUint16;
    case 4:
      return Datatype::kInt16;
    case 5:
      return Datatype::kUint32;
    case 6:
      return Datatype::kInt32;
    case 7:
      return Datatype::kFloat32;
    case 8:
      return Datatype::kFloat64;
    default:
      return Datatype::kUnknown;
  }
}

/// Skip a field whose value we do not consume, given its wire type.
[[nodiscard]] bool skipField(gpio::CodedInputStream& in, uint32_t wire_type) {
  switch (wire_type) {
    case kWireVarint: {
      uint64_t v = 0;
      return in.ReadVarint64(&v);
    }
    case kWireI64: {
      uint64_t v = 0;
      return in.ReadLittleEndian64(&v);
    }
    case kWireLen: {
      uint32_t len = 0;
      return in.ReadVarint32(&len) && in.Skip(static_cast<int>(len));
    }
    case kWireI32: {
      uint32_t v = 0;
      return in.ReadLittleEndian32(&v);
    }
    default:
      return false;  // groups (3/4) are not used by foxglove schemas.
  }
}

[[nodiscard]] bool readDouble(gpio::CodedInputStream& in, double& out) {
  uint64_t bits = 0;
  if (!in.ReadLittleEndian64(&bits)) {
    return false;
  }
  out = std::bit_cast<double>(bits);
  return true;
}

/// Parse a google.protobuf.Timestamp submessage of `len` bytes into nanoseconds.
[[nodiscard]] bool readTimestampNs(gpio::CodedInputStream& in, uint32_t len, int64_t& ts_ns) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  int64_t seconds = 0;
  int64_t nanos = 0;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == 1 && wt == kWireVarint) {
      uint64_t v = 0;
      if (!in.ReadVarint64(&v)) {
        return false;
      }
      seconds = static_cast<int64_t>(v);
    } else if (field == 2 && wt == kWireVarint) {
      uint64_t v = 0;
      if (!in.ReadVarint64(&v)) {
        return false;
      }
      nanos = static_cast<int64_t>(static_cast<int32_t>(v));
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  ts_ns = seconds * 1'000'000'000LL + nanos;
  return true;
}

/// Parse a foxglove.Pose submessage to decide whether it deviates from
/// identity. Defaults are identity (zero translation, unit quaternion), so an
/// omitted/empty pose reads as identity.
[[nodiscard]] bool readPoseIdentity(gpio::CodedInputStream& in, uint32_t len, bool& is_identity) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  double px = 0, py = 0, pz = 0;
  double qx = 0, qy = 0, qz = 0, qw = 1.0;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == 1 && wt == kWireLen) {  // position (Vector3{x=1,y=2,z=3})
      uint32_t sub_len = 0;
      if (!in.ReadVarint32(&sub_len)) {
        return false;
      }
      const auto sub = in.PushLimit(static_cast<int>(sub_len));
      uint32_t t = 0;
      while ((t = in.ReadTag()) != 0) {
        const int sf = static_cast<int>(t >> 3);
        const uint32_t swt = t & 0x7u;
        if (swt == kWireI64 && sf >= 1 && sf <= 3) {
          double d = 0;
          if (!readDouble(in, d)) {
            return false;
          }
          (sf == 1 ? px : sf == 2 ? py : pz) = d;
        } else if (!skipField(in, swt)) {
          return false;
        }
      }
      in.PopLimit(sub);
    } else if (field == 2 && wt == kWireLen) {  // orientation (Quaternion{x=1,y=2,z=3,w=4})
      uint32_t sub_len = 0;
      if (!in.ReadVarint32(&sub_len)) {
        return false;
      }
      const auto sub = in.PushLimit(static_cast<int>(sub_len));
      uint32_t t = 0;
      while ((t = in.ReadTag()) != 0) {
        const int sf = static_cast<int>(t >> 3);
        const uint32_t swt = t & 0x7u;
        if (swt == kWireI64 && sf >= 1 && sf <= 4) {
          double d = 0;
          if (!readDouble(in, d)) {
            return false;
          }
          (sf == 1 ? qx : sf == 2 ? qy : sf == 3 ? qz : qw) = d;
        } else if (!skipField(in, swt)) {
          return false;
        }
      }
      in.PopLimit(sub);
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  is_identity = (px == 0.0 && py == 0.0 && pz == 0.0 && qx == 0.0 && qy == 0.0 && qz == 0.0 && qw == 1.0);
  return true;
}

/// Reads one occurrence of a `repeated double` field. proto3 packs repeated
/// scalars by default (LEN of raw little-endian doubles), but a spec-compliant
/// parser must also accept the unpacked encoding (one I64 record per element).
[[nodiscard]] bool readRepeatedDouble(gpio::CodedInputStream& in, uint32_t wire_type, std::vector<double>& out) {
  if (wire_type == kWireI64) {  // unpacked: a single element
    double v = 0;
    if (!readDouble(in, v)) {
      return false;
    }
    out.push_back(v);
    return true;
  }
  if (wire_type != kWireLen) {
    return false;
  }
  uint32_t len = 0;
  if (!in.ReadVarint32(&len) || (len % sizeof(double)) != 0) {
    return false;
  }
  const auto limit = in.PushLimit(static_cast<int>(len));
  const size_t count = len / sizeof(double);
  // Cap the reserve: a corrupt LEN must not drive a huge allocation before the
  // first read fails. Honest payloads beyond the cap just grow geometrically.
  constexpr size_t kReserveCap = 4096;  // rays; generous for any physical lidar
  out.reserve(out.size() + std::min(count, kReserveCap));
  for (size_t i = 0; i < count; ++i) {
    double v = 0;
    if (!readDouble(in, v)) {
      return false;
    }
    out.push_back(v);
  }
  in.PopLimit(limit);
  return true;
}

/// Parse one PackedElementField submessage of `len` bytes.
[[nodiscard]] bool readPackedElementField(gpio::CodedInputStream& in, uint32_t len, PJ::sdk::PointField& out) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == kPefName && wt == kWireLen) {
      uint32_t s = 0;
      if (!in.ReadVarint32(&s) || !in.ReadString(&out.name, static_cast<int>(s))) {
        return false;
      }
    } else if (field == kPefOffset && wt == kWireI32) {
      uint32_t off = 0;
      if (!in.ReadLittleEndian32(&off)) {
        return false;
      }
      out.offset = off;
    } else if (field == kPefType && wt == kWireVarint) {
      uint64_t t = 0;
      if (!in.ReadVarint64(&t)) {
        return false;
      }
      out.datatype = mapFoxgloveNumericType(t);
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  out.count = 1;  // Foxglove PackedElementField has no `count`; one element per field.
  return true;
}

}  // namespace

PJ::Expected<FoxglovePointCloudDecode> deserializeFoxglovePointCloudView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.PointCloud: message too large"));
  }

  FoxglovePointCloudDecode result;
  PJ::sdk::PointCloud& cloud = result.cloud;

  gpio::CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());

  int64_t ts_ns = 0;
  uint32_t point_stride = 0;
  PJ::Span<const uint8_t> data_span;

  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    switch (field) {
      case kFieldTimestamp: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.PointCloud: bad timestamp wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len) || !readTimestampNs(in, len, ts_ns)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read timestamp"));
        }
        break;
      }
      case kFieldFrameId: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.PointCloud: bad frame_id wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len) || !in.ReadString(&cloud.frame_id, static_cast<int>(len))) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read frame_id"));
        }
        break;
      }
      case kFieldPose: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.PointCloud: bad pose wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read pose length"));
        }
        result.has_pose = true;
        if (!readPoseIdentity(in, len, result.pose_is_identity)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read pose"));
        }
        break;
      }
      case kFieldPointStride: {
        if (wt != kWireI32) {
          return PJ::unexpected(std::string("foxglove.PointCloud: bad point_stride wire type"));
        }
        if (!in.ReadLittleEndian32(&point_stride)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read point_stride"));
        }
        break;
      }
      case kFieldFields: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.PointCloud: bad fields wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read field length"));
        }
        PJ::sdk::PointField pf;
        if (!readPackedElementField(in, len, pf)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read PackedElementField"));
        }
        cloud.fields.push_back(std::move(pf));
        break;
      }
      case kFieldData: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.PointCloud: bad data wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: failed to read data length"));
        }
        if (len > 0) {
          // Zero-copy: alias the packed-point bytes in place instead of copying
          // (a lidar scan is multi-MB). GetDirectBufferPointer hands back a
          // pointer into the original `data` buffer; the BufferAnchor keeps it
          // alive past this call.
          const void* ptr = nullptr;
          int avail = 0;
          if (!in.GetDirectBufferPointer(&ptr, &avail) || avail < static_cast<int>(len)) {
            return PJ::unexpected(std::string("foxglove.PointCloud: data not contiguous"));
          }
          data_span = PJ::Span<const uint8_t>(static_cast<const uint8_t*>(ptr), len);
          if (!in.Skip(static_cast<int>(len))) {
            return PJ::unexpected(std::string("foxglove.PointCloud: failed to skip data"));
          }
        }
        break;
      }
      default:
        if (!skipField(in, wt)) {
          return PJ::unexpected(std::string("foxglove.PointCloud: malformed message"));
        }
        break;
    }
  }

  // Map + synthesize the canonical fields. Foxglove is always a flat (height=1),
  // little-endian, dense point list with no row padding, so the geometry that
  // PointCloud2 carries explicitly is derived here.
  cloud.point_step = point_stride;
  cloud.height = 1;
  cloud.width = point_stride > 0 ? static_cast<uint32_t>(data_span.size() / point_stride) : 0;
  cloud.row_step = cloud.width * cloud.point_step;
  cloud.is_bigendian = false;
  cloud.is_dense = true;
  cloud.data = data_span;
  cloud.anchor = std::move(anchor);
  cloud.timestamp_ns = ts_ns;

  return result;
}

PJ::Expected<FoxgloveLaserScanDecode> deserializeFoxgloveLaserScan(
    const uint8_t* data, size_t size, PJ::laser_scan::LaserScanProjector& projector) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.LaserScan: message too large"));
  }

  FoxgloveLaserScanDecode result;

  gpio::CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());

  int64_t ts_ns = 0;
  std::string frame_id;
  std::vector<double> ranges;
  std::vector<double> intensities;

  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    switch (field) {
      case kLsTimestamp: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.LaserScan: bad timestamp wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len) || !readTimestampNs(in, len, ts_ns)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read timestamp"));
        }
        break;
      }
      case kLsFrameId: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.LaserScan: bad frame_id wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len) || !in.ReadString(&frame_id, static_cast<int>(len))) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read frame_id"));
        }
        break;
      }
      case kLsPose: {
        if (wt != kWireLen) {
          return PJ::unexpected(std::string("foxglove.LaserScan: bad pose wire type"));
        }
        uint32_t len = 0;
        if (!in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read pose length"));
        }
        result.has_pose = true;
        if (!readPoseIdentity(in, len, result.pose_is_identity)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read pose"));
        }
        break;
      }
      case kLsStartAngle: {
        if (wt != kWireI64 || !readDouble(in, result.start_angle)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read start_angle"));
        }
        break;
      }
      case kLsEndAngle: {
        if (wt != kWireI64 || !readDouble(in, result.end_angle)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read end_angle"));
        }
        break;
      }
      case kLsRanges: {
        if (!readRepeatedDouble(in, wt, ranges)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read ranges"));
        }
        break;
      }
      case kLsIntensities: {
        if (!readRepeatedDouble(in, wt, intensities)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: failed to read intensities"));
        }
        break;
      }
      default:
        if (!skipField(in, wt)) {
          return PJ::unexpected(std::string("foxglove.LaserScan: malformed message"));
        }
        break;
    }
  }

  result.ray_count = ranges.size();

  // Rays sit at equally-spaced angles between start_angle and end_angle
  // (inclusive). Foxglove carries no range bounds, so ScanParams leaves them
  // unset and only non-finite ranges drop.
  const PJ::laser_scan::ScanParams params{
      .angle_min = result.start_angle,
      .angle_increment =
          ranges.size() > 1 ? (result.end_angle - result.start_angle) / static_cast<double>(ranges.size() - 1) : 0.0,
  };
  result.cloud = projector.project(
      params, PJ::Span<const double>(ranges.data(), ranges.size()),
      PJ::Span<const double>(intensities.data(), intensities.size()));
  result.cloud.frame_id = std::move(frame_id);
  result.cloud.timestamp_ns = ts_ns;

  return result;
}

PJ::Expected<FoxgloveLaserScanInfo> readFoxgloveLaserScanInfo(const uint8_t* data, size_t size) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.LaserScan: message too large"));
  }

  FoxgloveLaserScanInfo info;
  gpio::CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());

  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == kLsTimestamp && wt == kWireLen) {
      uint32_t len = 0;
      if (!in.ReadVarint32(&len) || !readTimestampNs(in, len, info.timestamp_ns)) {
        return PJ::unexpected(std::string("foxglove.LaserScan: failed to read timestamp"));
      }
    } else if (field == kLsFrameId && wt == kWireLen) {
      uint32_t len = 0;
      if (!in.ReadVarint32(&len) || !in.ReadString(&info.frame_id, static_cast<int>(len))) {
        return PJ::unexpected(std::string("foxglove.LaserScan: failed to read frame_id"));
      }
    } else if (field == kLsStartAngle && wt == kWireI64) {
      if (!readDouble(in, info.start_angle)) {
        return PJ::unexpected(std::string("foxglove.LaserScan: failed to read start_angle"));
      }
    } else if (field == kLsEndAngle && wt == kWireI64) {
      if (!readDouble(in, info.end_angle)) {
        return PJ::unexpected(std::string("foxglove.LaserScan: failed to read end_angle"));
      }
    } else if (field == kLsRanges && wt == kWireI64) {
      // Unpacked encoding: one I64 record per ray.
      if (!skipField(in, wt)) {
        return PJ::unexpected(std::string("foxglove.LaserScan: failed to read ranges"));
      }
      ++info.num_ranges;
    } else if (field == kLsRanges && wt == kWireLen) {
      // Packed encoding: the LEN alone gives the ray count; skip the bytes.
      uint32_t len = 0;
      if (!in.ReadVarint32(&len) || (len % sizeof(double)) != 0 || !in.Skip(static_cast<int>(len))) {
        return PJ::unexpected(std::string("foxglove.LaserScan: failed to read ranges"));
      }
      info.num_ranges += len / sizeof(double);
    } else if (!skipField(in, wt)) {
      return PJ::unexpected(std::string("foxglove.LaserScan: malformed message"));
    }
  }
  return info;
}

}  // namespace pj_protobuf
