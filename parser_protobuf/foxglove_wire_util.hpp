#pragma once
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Protobuf wire-level helpers shared by the hand-rolled foxglove codecs that
// scan a known layout with CodedInputStream (VoxelGrid, Grid). The nested
// geometry messages (Timestamp, Vector2/3, Quaternion, Pose) carry no frame_id
// and are never renumbered by the variant schemas PR #153 handles, so their
// field numbers stay hardcoded here.

#include <google/protobuf/io/coded_stream.h>

#include <bit>
#include <cstdint>
#include <pj_base/builtin/frame_transforms.hpp>

namespace pj_protobuf::wire {

namespace gpio = google::protobuf::io;

// Protobuf wire types.
constexpr uint32_t kWireVarint = 0;
constexpr uint32_t kWireI64 = 1;
constexpr uint32_t kWireLen = 2;
constexpr uint32_t kWireI32 = 5;

/// Skip a field whose value we do not consume, given its wire type.
[[nodiscard]] inline bool skipField(gpio::CodedInputStream& in, uint32_t wire_type) {
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

[[nodiscard]] inline bool readDouble(gpio::CodedInputStream& in, double& out) {
  uint64_t bits = 0;
  if (!in.ReadLittleEndian64(&bits)) {
    return false;
  }
  out = std::bit_cast<double>(bits);
  return true;
}

/// Read a fixed32 scalar field into `out` (the Foxglove counts/strides are all
/// fixed32).
[[nodiscard]] inline bool readFixed32(gpio::CodedInputStream& in, uint32_t wire_type, uint32_t& out) {
  return wire_type == kWireI32 && in.ReadLittleEndian32(&out);
}

/// Parse a google.protobuf.Timestamp submessage of `len` bytes into nanoseconds.
[[nodiscard]] inline bool readTimestampNs(gpio::CodedInputStream& in, uint32_t len, int64_t& ts_ns) {
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

/// Parse a foxglove.Vector2 submessage (x=1, y=2 as double) of `len` bytes.
[[nodiscard]] inline bool readVector2(gpio::CodedInputStream& in, uint32_t len, PJ::sdk::Vector2& out) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (wt == kWireI64 && (field == 1 || field == 2)) {
      double d = 0;
      if (!readDouble(in, d)) {
        return false;
      }
      (field == 1 ? out.x : out.y) = d;
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  return true;
}

/// Parse a foxglove.Vector3 submessage (x=1, y=2, z=3 as double) of `len` bytes.
[[nodiscard]] inline bool readVector3(gpio::CodedInputStream& in, uint32_t len, PJ::sdk::Vector3& out) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (wt == kWireI64 && field >= 1 && field <= 3) {
      double d = 0;
      if (!readDouble(in, d)) {
        return false;
      }
      (field == 1 ? out.x : field == 2 ? out.y : out.z) = d;
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  return true;
}

/// Parse a foxglove.Pose submessage (position Vector3 = 1, orientation
/// Quaternion = 2) of `len` bytes into `out`. `out` is pre-seeded to identity so
/// an omitted position/orientation reads as identity.
[[nodiscard]] inline bool readPose(gpio::CodedInputStream& in, uint32_t len, PJ::sdk::Pose& out) {
  out.position = {.x = 0.0, .y = 0.0, .z = 0.0};
  out.orientation = {.x = 0.0, .y = 0.0, .z = 0.0, .w = 1.0};
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == 1 && wt == kWireLen) {  // position (Vector3)
      uint32_t sub_len = 0;
      if (!in.ReadVarint32(&sub_len) || !readVector3(in, sub_len, out.position)) {
        return false;
      }
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
          (sf == 1   ? out.orientation.x
           : sf == 2 ? out.orientation.y
           : sf == 3 ? out.orientation.z
                     : out.orientation.w) = d;
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
  return true;
}

}  // namespace pj_protobuf::wire
