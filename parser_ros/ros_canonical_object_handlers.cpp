/**
 * @file ros_canonical_object_handlers.cpp
 * @brief Per-schema canonical-object decoders. One parse<X>() entry per
 *        schema returns an Expected<CanonicalObject> ready for ObjectStore
 *        ingestion: deserializer setup + body walk + zero-copy variant
 *        wrap, all inline.
 *
 * Zero-copy strategy (iteration 3):
 *   - The parser receives a PayloadView (bytes + anchor). The bytes span
 *     points into the host's payload buffer; the anchor is the shared
 *     ownership token that keeps it alive.
 *   - For the bulk byte array of each schema (Image::data, PointCloud2::data,
 *     CompressedImage::data) we DO NOT allocate a new vector and copy.
 *     We compute a Span<const uint8_t> into the original payload at the
 *     correct offset+length and propagate `payload.anchor` into the result.
 *   - When the wire format has BGR ordering or per-row padding, that is
 *     reflected in the canonical fields (PixelFormat::kBGR*, row_step) so
 *     the consumer handles them — no parser-side conversion.
 *
 * The scalar-side companion lives in ros_parser.cpp:
 * parseScalarsDiscardingLargeArrays() reuses flattenGeneric with the bulk
 * array policy forced to DISCARD, so small metadata fields show up as
 * scalar columns while the data[] blob is dropped automatically.
 */

#include "ros_parser_internal.hpp"

#include <cstring>
#include <unordered_map>

namespace ros_parser_detail {

// ---------------------------------------------------------------------------
// sensor_msgs/Image
//
// CDR layout (after the 4-byte CDR header consumed by the deserializer):
//   header.stamp.sec        uint32
//   header.stamp.nanosec    uint32
//   header.frame_id         string
//   height                  uint32
//   width                   uint32
//   encoding                string (e.g. "rgb8", "bgr8", "mono8", "mono16", "16UC1", …)
//   is_bigendian            uint8
//   step                    uint32
//   data                    uint8[height*step]
// ---------------------------------------------------------------------------

namespace {

struct RosEncodingMap {
  PJ::sdk::PixelFormat format;
  uint32_t bytes_per_pixel;
};

// Mapping ROS encoding string → canonical PJ pixel format. BGR variants map
// straight to kBGR888/kBGRA8888 (no R↔B swap); the consumer handles channel
// order via texture-format selection.
const std::unordered_map<std::string, RosEncodingMap>& kRosImageEncodings() {
  static const std::unordered_map<std::string, RosEncodingMap> kMap = {
      {"rgb8", {PJ::sdk::PixelFormat::kRGB888, 3}},
      {"rgba8", {PJ::sdk::PixelFormat::kRGBA8888, 4}},
      {"bgr8", {PJ::sdk::PixelFormat::kBGR888, 3}},
      {"bgra8", {PJ::sdk::PixelFormat::kBGRA8888, 4}},
      {"mono8", {PJ::sdk::PixelFormat::kMono8, 1}},
      {"mono16", {PJ::sdk::PixelFormat::kMono16, 2}},
      {"16UC1", {PJ::sdk::PixelFormat::kMono16, 2}},
  };
  return kMap;
}

}  // namespace

PJ::Expected<PJ::sdk::CanonicalObject> RosParser::parseImage(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  if (!parser_.has_value()) return PJ::unexpected(std::string("no schema bound"));
  ensureDeserializer();
  deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

  (void)readHeader();
  uint32_t height = deserializer_->deserializeUInt32();
  uint32_t width = deserializer_->deserializeUInt32();
  std::string encoding;
  deserializer_->deserializeString(encoding);
  uint8_t is_be = deserializer_->deserialize(RosMsgParser::UINT8).convert<uint8_t>();
  uint32_t step = deserializer_->deserializeUInt32();

  auto it = kRosImageEncodings().find(encoding);
  if (it == kRosImageEncodings().end()) {
    return PJ::unexpected(std::string("unsupported ROS encoding: ") + encoding);
  }
  const auto& enc = it->second;

  uint32_t data_len = deserializer_->deserializeUInt32();
  if (data_len < step * height) {
    return PJ::unexpected(std::string("Image data[] truncated"));
  }
  if (step < width * enc.bytes_per_pixel) {
    return PJ::unexpected(std::string("Image step smaller than width*bpp"));
  }
  const uint8_t* src_data = deserializer_->getCurrentPtr();

  // Zero-copy: the canonical Image holds a Span into the original payload
  // buffer plus the same anchor. No allocation, no copy. row_step preserves
  // any per-row padding the source had — consumer handles it. BGR encodings
  // stay as kBGR* — consumer handles the swap (free on GPU).
  return PJ::sdk::CanonicalObject{PJ::sdk::Image{
      .width = width,
      .height = height,
      .pixel_format = enc.format,
      .row_step = step,
      .is_bigendian = (is_be != 0),
      .pixels = PJ::Span<const uint8_t>(src_data, static_cast<size_t>(step) * height),
      .anchor = payload.anchor,
      .timestamp_ns = ts,
  }};
}

// ---------------------------------------------------------------------------
// sensor_msgs/CompressedImage
//
// CDR layout:
//   header                  Header (Time + frame_id)
//   format                  string (e.g. "jpeg", "png", "16UC1; compressedDepth png")
//   data                    uint8[]   ← compressed bytes, plus an optional
//                                       12-byte compressedDepth mini-header
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::CanonicalObject> RosParser::parseCompressedImage(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  if (!parser_.has_value()) return PJ::unexpected(std::string("no schema bound"));
  ensureDeserializer();
  deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

  (void)readHeader();
  std::string format;
  deserializer_->deserializeString(format);

  uint32_t data_len = deserializer_->deserializeUInt32();
  if (data_len > payload.bytes.size()) {
    return PJ::unexpected(std::string("CompressedImage data[] truncated"));
  }
  const uint8_t* src = deserializer_->getCurrentPtr();

  PJ::sdk::CompressedImage::Format fmt = PJ::sdk::CompressedImage::Format::kUnknown;
  PJ::sdk::CompressedImage::Extras extras;
  size_t blob_offset = 0;
  uint32_t blob_size = data_len;

  if (format.find("compressedDepth") != std::string::npos) {
    if (data_len < 12) {
      return PJ::unexpected(std::string("compressedDepth data[] too short for header"));
    }
    // Mini-header: uint32 format (ignored), float depth_min, float depth_max.
    float depth_min = 0.0f;
    float depth_max = 0.0f;
    std::memcpy(&depth_min, src + 4, sizeof(float));
    std::memcpy(&depth_max, src + 8, sizeof(float));
    extras.compressed_depth_min = depth_min;
    extras.compressed_depth_max = depth_max;
    fmt = PJ::sdk::CompressedImage::Format::kPNG;  // compressedDepth always wraps PNG
    blob_offset = 12;
    blob_size = data_len - 12;
  } else if (format.find("jpeg") != std::string::npos) {
    fmt = PJ::sdk::CompressedImage::Format::kJPEG;
  } else if (format == "png") {
    fmt = PJ::sdk::CompressedImage::Format::kPNG;
  } else {
    return PJ::unexpected(std::string("unsupported CompressedImage format: ") + format);
  }

  // Zero-copy: span slices the payload at [src+blob_offset, +blob_size).
  // Stripping the 12-byte compressedDepth header is a Span::subspan, not a
  // realloc.
  PJ::sdk::CompressedImage out;
  out.format = fmt;
  out.bytes = PJ::Span<const uint8_t>(src + blob_offset, blob_size);
  out.anchor = payload.anchor;
  out.timestamp_ns = ts;
  out.extras = extras;
  return PJ::sdk::CanonicalObject{std::move(out)};
}

// ---------------------------------------------------------------------------
// sensor_msgs/PointCloud2
//
// CDR layout:
//   header                  Header
//   height                  uint32
//   width                   uint32
//   fields                  PointField[]
//     each PointField:
//       name                string
//       offset              uint32
//       datatype            uint8     (1=INT8, 2=UINT8, 3=INT16, 4=UINT16,
//                                      5=INT32, 6=UINT32, 7=FLOAT32, 8=FLOAT64)
//       count               uint32
//   is_bigendian            uint8
//   point_step              uint32
//   row_step                uint32
//   data                    uint8[height*row_step]
//   is_dense                uint8
// ---------------------------------------------------------------------------

namespace {

inline PJ::sdk::PointField::Datatype mapRosPointDatatype(uint8_t dt) {
  switch (dt) {
    case 1:
      return PJ::sdk::PointField::Datatype::kInt8;
    case 2:
      return PJ::sdk::PointField::Datatype::kUint8;
    case 3:
      return PJ::sdk::PointField::Datatype::kInt16;
    case 4:
      return PJ::sdk::PointField::Datatype::kUint16;
    case 5:
      return PJ::sdk::PointField::Datatype::kInt32;
    case 6:
      return PJ::sdk::PointField::Datatype::kUint32;
    case 7:
      return PJ::sdk::PointField::Datatype::kFloat32;
    case 8:
      return PJ::sdk::PointField::Datatype::kFloat64;
    default:
      return PJ::sdk::PointField::Datatype::kUnknown;
  }
}

}  // namespace

PJ::Expected<PJ::sdk::CanonicalObject> RosParser::parsePointCloud(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  if (!parser_.has_value()) return PJ::unexpected(std::string("no schema bound"));
  ensureDeserializer();
  deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

  (void)readHeader();
  uint32_t height = deserializer_->deserializeUInt32();
  uint32_t width = deserializer_->deserializeUInt32();

  uint32_t fields_count = deserializer_->deserializeUInt32();
  std::vector<PJ::sdk::PointField> fields;
  fields.reserve(fields_count);
  for (uint32_t i = 0; i < fields_count; ++i) {
    std::string name;
    deserializer_->deserializeString(name);
    uint32_t offset = deserializer_->deserializeUInt32();
    uint8_t dt_raw = deserializer_->deserialize(RosMsgParser::UINT8).convert<uint8_t>();
    uint32_t count = deserializer_->deserializeUInt32();
    fields.push_back(PJ::sdk::PointField{
        .name = std::move(name),
        .offset = offset,
        .datatype = mapRosPointDatatype(dt_raw),
        .count = count,
    });
  }

  uint8_t is_be = deserializer_->deserialize(RosMsgParser::UINT8).convert<uint8_t>();
  uint32_t point_step = deserializer_->deserializeUInt32();
  uint32_t row_step = deserializer_->deserializeUInt32();

  uint32_t data_len = deserializer_->deserializeUInt32();
  const uint8_t* src_data = deserializer_->getCurrentPtr();
  // is_dense follows data[] in the CDR layout. The exact placement of the
  // post-data cursor depends on how rosx_introspection's deserializer
  // advances over the array contents; verify against the test MCAPs when
  // wiring up the consumer side.
  // TODO(rfc): verify is_dense read after data[] with the actual
  // NanoCDR_Deserializer semantics. For now, default to "true" (dense).
  bool is_dense = true;

  // Zero-copy: data is a Span over [src_data, src_data+data_len) sharing
  // the payload anchor. For a PointCloud2 with a few MB of points this is
  // the win — no per-message alloc/copy on the hot path.
  return PJ::sdk::CanonicalObject{PJ::sdk::PointCloud{
      .width = width,
      .height = height,
      .point_step = point_step,
      .row_step = row_step,
      .is_bigendian = (is_be != 0),
      .is_dense = is_dense,
      .fields = std::move(fields),
      .data = PJ::Span<const uint8_t>(src_data, data_len),
      .anchor = payload.anchor,
      .timestamp_ns = ts,
  }};
}

}  // namespace ros_parser_detail
