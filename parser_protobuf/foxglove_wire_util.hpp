#pragma once
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Protobuf wire-level helpers shared by the hand-rolled foxglove codecs that
// scan a known layout with CodedInputStream (PointCloud, VoxelGrid, Grid). The nested
// geometry messages (Timestamp, Vector2/3, Quaternion, Pose) carry no frame_id
// and are never renumbered by the variant schemas PR #153 handles, so their
// field numbers stay hardcoded here.

#include <google/protobuf/io/coded_stream.h>

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <pj_base/builtin/frame_transforms.hpp>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_grid_map/grid_map_transcoder.hpp>

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

/// Parse a submessage of `len` bytes whose fields 1..N are doubles, storing
/// field i into `slots[i-1]` (Vector2/3, Quaternion). Omitted fields keep their
/// current value; anything else is skipped.
[[nodiscard]] inline bool readDoubleFields(
    gpio::CodedInputStream& in, uint32_t len, std::initializer_list<double*> slots) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (wt == kWireI64 && field >= 1 && field <= static_cast<int>(slots.size())) {
      if (!readDouble(in, *slots.begin()[field - 1])) {
        return false;
      }
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  return true;
}

/// Parse a foxglove.Vector2 submessage (x=1, y=2 as double) of `len` bytes.
[[nodiscard]] inline bool readVector2(gpio::CodedInputStream& in, uint32_t len, PJ::sdk::Vector2& out) {
  return readDoubleFields(in, len, {&out.x, &out.y});
}

/// Parse a foxglove.Vector3 submessage (x=1, y=2, z=3 as double) of `len` bytes.
[[nodiscard]] inline bool readVector3(gpio::CodedInputStream& in, uint32_t len, PJ::sdk::Vector3& out) {
  return readDoubleFields(in, len, {&out.x, &out.y, &out.z});
}

/// Parse a foxglove.Pose submessage (position Vector3 = 1, orientation
/// Quaternion{x=1,y=2,z=3,w=4} = 2) of `len` bytes into `out`. `out` is
/// pre-seeded to identity so an omitted position/orientation reads as identity.
[[nodiscard]] inline bool readPose(gpio::CodedInputStream& in, uint32_t len, PJ::sdk::Pose& out) {
  out.position = {.x = 0.0, .y = 0.0, .z = 0.0};
  out.orientation = {.x = 0.0, .y = 0.0, .z = 0.0, .w = 1.0};
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    uint32_t sub_len = 0;
    if (field == 1 && wt == kWireLen) {
      if (!in.ReadVarint32(&sub_len) || !readVector3(in, sub_len, out.position)) {
        return false;
      }
    } else if (field == 2 && wt == kWireLen) {
      auto& q = out.orientation;
      if (!in.ReadVarint32(&sub_len) || !readDoubleFields(in, sub_len, {&q.x, &q.y, &q.z, &q.w})) {
        return false;
      }
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  return true;
}

/// Parse one foxglove.PackedElementField submessage of `len` bytes. The field
/// numbers come from the owning codec's descriptor-resolved struct (name /
/// offset / type). Foxglove carries no `count`: one element per field.
[[nodiscard]] inline bool readPackedElementField(
    gpio::CodedInputStream& in, uint32_t len, PJ::sdk::PointField& out, int name_field, int offset_field,
    int type_field) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == name_field && wt == kWireLen) {
      uint32_t s = 0;
      if (!in.ReadVarint32(&s) || !in.ReadString(&out.name, static_cast<int>(s))) {
        return false;
      }
    } else if (field == offset_field && wt == kWireI32) {
      if (!in.ReadLittleEndian32(&out.offset)) {
        return false;
      }
    } else if (field == type_field && wt == kWireVarint) {
      uint64_t t = 0;
      if (!in.ReadVarint64(&t)) {
        return false;
      }
      out.datatype = PJ::grid_map::foxgloveNumericTypeToPointField(t);
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  out.count = 1;
  return true;
}

}  // namespace pj_protobuf::wire
