// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
#pragma once

// Canonical point-cloud colour normalization, shared by parser_ros and
// parser_protobuf so both emit the SAME canonical colour representation and the
// host (pj_scene3D) never has to know per-source byte order.
//
// Canonical contract:
//   colour, when present, is a SINGLE PointField{ name:"rgba", datatype:kUint32 }
//   (or "rgb" when the source has no alpha). The 4 bytes at the field's offset are
//   R, G, B, A in increasing address order — i.e. a little-endian uint32 read gives
//   R in the low byte. The host reads r=v&0xFF, g=(v>>8)&0xFF, b=(v>>16)&0xFF,
//   a=(v>>24)&0xFF, and forces alpha=255 for an "rgb" field.
//
// Two source shapes converge here:
//   * Foxglove: four separate uint8 channels red/green/blue/alpha, already laid out
//     R,G,B,A — collapseSeparateColorChannels() does a pure metadata rewrite (zero-copy).
//   * ROS PCL: a single packed FLOAT32 "rgb" / UINT32 "rgba" in 0x00RRGGBB order
//     (B in the low byte) — that repack lives in parser_ros (it owns the buffer); this
//     header only carries the shared field-name + the separate-channel collapse.

#include <cstdint>
#include <memory>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_base/span.hpp>
#include <string>
#include <vector>

namespace pj::pointcloud_color {

// Canonical packed-colour field names (alpha present vs. absent).
inline constexpr const char* kCanonicalRgbaField = "rgba";
inline constexpr const char* kCanonicalRgbField = "rgb";

// Collapse separate uint8 colour channels (red/green/blue[/alpha]) into one packed
// uint32 colour field WITHOUT touching the point buffer: the bytes are already R,G,B,A
// in increasing address, so this is a pure field-metadata rewrite that preserves
// zero-copy. The packed field is named "rgba" (alpha present) or "rgb" (no alpha; the
// host forces alpha=255) and appended after the surviving fields.
//
// Leaves the fields unchanged (returns false) unless red, green, blue are all present,
// all uint8, and contiguous (offsets o, o+1, o+2), and either alpha is uint8 at o+3, or
// there is no alpha field at all AND the point has a 4th byte to read (o+4 <= point_step).
// An unusual alpha layout (present but not at o+3) is left for the host's defensive path.
inline bool collapseSeparateColorChannels(PJ::sdk::PointCloud& cloud) {
  using DT = PJ::sdk::PointField::Datatype;
  const PJ::sdk::PointField* red = nullptr;
  const PJ::sdk::PointField* green = nullptr;
  const PJ::sdk::PointField* blue = nullptr;
  const PJ::sdk::PointField* alpha = nullptr;
  for (const auto& field : cloud.fields) {
    if (field.name == "red") {
      red = &field;
    } else if (field.name == "green") {
      green = &field;
    } else if (field.name == "blue") {
      blue = &field;
    } else if (field.name == "alpha") {
      alpha = &field;
    }
  }

  const auto is_u8 = [](const PJ::sdk::PointField* field) { return field != nullptr && field->datatype == DT::kUint8; };
  if (!is_u8(red) || !is_u8(green) || !is_u8(blue)) {
    return false;
  }
  const uint32_t base = red->offset;
  if (green->offset != base + 1 || blue->offset != base + 2) {
    return false;  // not contiguous R,G,B
  }
  const bool alpha_ok = is_u8(alpha) && alpha->offset == base + 3;
  if (alpha != nullptr && !alpha_ok) {
    return false;  // unusual alpha layout; leave the host's defensive path to handle it
  }
  const bool has_alpha = alpha_ok;
  if (!has_alpha && static_cast<uint64_t>(base) + 4u > static_cast<uint64_t>(cloud.point_step)) {
    return false;  // no 4th byte to read as a packed uint32
  }

  std::vector<PJ::sdk::PointField> kept;
  kept.reserve(cloud.fields.size());
  for (const auto& field : cloud.fields) {
    const bool is_color =
        field.name == "red" || field.name == "green" || field.name == "blue" || (has_alpha && field.name == "alpha");
    if (!is_color) {
      kept.push_back(field);
    }
  }
  PJ::sdk::PointField packed;
  packed.name = has_alpha ? kCanonicalRgbaField : kCanonicalRgbField;
  packed.offset = base;
  packed.datatype = DT::kUint32;
  packed.count = 1;
  kept.push_back(std::move(packed));
  cloud.fields = std::move(kept);
  return true;
}

// Repack a PCL-style packed colour field — a single FLOAT32 "rgb" or UINT32 "rgba"
// whose bytes are 0x00RRGGBB little-endian (B in the low byte) — into the canonical
// R,G,B,A byte order. Because the byte order differs from canonical, this rewrites the
// point buffer: it allocates a fresh copy (which the cloud's anchor then owns) and
// reorders the 4 colour bytes per point, then rewrites the field to {name:"rgba",
// datatype:kUint32}. Zero-copy is sacrificed ONLY for colour clouds; plain clouds never
// reach here. No-op (returns false) when there is no packed colour field, the cloud is
// big-endian, the field doesn't fit the point, or the geometry is malformed.
inline bool repackPclPackedColor(PJ::sdk::PointCloud& cloud) {
  using DT = PJ::sdk::PointField::Datatype;
  PJ::sdk::PointField* packed = nullptr;
  for (auto& field : cloud.fields) {
    if ((field.name == "rgb" || field.name == "rgba") &&
        (field.datatype == DT::kFloat32 || field.datatype == DT::kUint32)) {
      packed = &field;
      break;
    }
  }
  if (packed == nullptr || cloud.is_bigendian) {
    return false;
  }
  const uint32_t offset = packed->offset;
  const uint64_t step = cloud.point_step;
  if (step < static_cast<uint64_t>(offset) + 4u) {
    return false;  // field doesn't fit the point
  }
  const uint64_t point_count = static_cast<uint64_t>(cloud.width) * static_cast<uint64_t>(cloud.height);
  if (cloud.data.size() < point_count * step) {
    return false;  // declared geometry exceeds the backing buffer
  }

  const bool has_alpha = (packed->name == "rgba");
  auto owned = std::make_shared<std::vector<uint8_t>>(cloud.data.data(), cloud.data.data() + cloud.data.size());
  for (uint64_t i = 0; i < point_count; ++i) {
    uint8_t* p = owned->data() + i * step + offset;
    // PCL little-endian memory: p[0]=B, p[1]=G, p[2]=R, p[3]=A (or 0 for "rgb").
    const uint8_t blue = p[0];
    const uint8_t green = p[1];
    const uint8_t red = p[2];
    const uint8_t alpha = has_alpha ? p[3] : 255;
    p[0] = red;
    p[1] = green;
    p[2] = blue;
    p[3] = alpha;
  }
  cloud.data = PJ::Span<const uint8_t>(owned->data(), owned->size());
  cloud.anchor = owned;
  packed->name = kCanonicalRgbaField;  // always carries alpha after the repack
  packed->datatype = DT::kUint32;
  packed->count = 1;
  return true;
}

// Normalize any recognized colour representation to the canonical packed "rgba"/"rgb"
// field. Tries the zero-copy separate-channel collapse first, then the PCL packed-colour
// repack. The single entry point both parsers call. Returns true when colour was found.
inline bool normalizeCanonicalColor(PJ::sdk::PointCloud& cloud) {
  return collapseSeparateColorChannels(cloud) || repackPclPackedColor(cloud);
}

}  // namespace pj::pointcloud_color
