/**
 * @file ros_canonical_object_handlers.cpp
 * @brief Per-schema canonical-object decoders. One parse<X>() entry per
 *        schema returns an Expected<CanonicalObject> ready for ObjectStore
 *        ingestion: deserializer setup + body walk + zero-copy variant
 *        wrap, all inline.
 *
 * Wire-format handling:
 *   - Reuses the parser's RosMsgParser::Deserializer (initialised in
 *     bindSchema based on use_ros1_). NanoCDR_Deserializer applies proper
 *     XCDR1 origin-relative alignment and honours the CDR encapsulation
 *     header's endianness flag; ROS_Deserializer reads tightly-packed
 *     little-endian ROS1 messages. The same handler code therefore works
 *     for both protocols — readHeader() already branches on isROS2() to
 *     pick up the ROS1-only seq field.
 *
 * Zero-copy strategy:
 *   - The parser receives a PayloadView (bytes + anchor). The bytes span
 *     points into the host's payload buffer; the anchor is the shared
 *     ownership token that keeps it alive.
 *   - For the bulk byte array of each schema (Image::data, PointCloud2::data,
 *     CompressedImage::data) we use Deserializer::deserializeByteSequence()
 *     which returns a Span<const uint8_t> over the original payload at the
 *     correct offset+length and advances the cursor past the body. We
 *     propagate `payload.anchor` into the result so the bytes outlive the
 *     parse call.
 *   - When the wire format has BGR ordering or per-row padding, that is
 *     reflected in the canonical fields (PixelFormat::kBGR*, row_step) so
 *     the consumer handles them — no parser-side conversion.
 *
 * The scalar-side companion lives in ros_parser.cpp:
 * parseScalarsDiscardingLargeArrays() reuses flattenGeneric with the bulk
 * array policy forced to DISCARD, so small metadata fields show up as
 * scalar columns while the data[] blob is dropped automatically.
 */

#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

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
      {"rgb8", {PJ::sdk::PixelFormat::kRGB888, 3}},  {"rgba8", {PJ::sdk::PixelFormat::kRGBA8888, 4}},
      {"bgr8", {PJ::sdk::PixelFormat::kBGR888, 3}},  {"bgra8", {PJ::sdk::PixelFormat::kBGRA8888, 4}},
      {"mono8", {PJ::sdk::PixelFormat::kMono8, 1}},  {"mono16", {PJ::sdk::PixelFormat::kMono16, 2}},
      {"16UC1", {PJ::sdk::PixelFormat::kMono16, 2}},
  };
  return kMap;
}

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

inline uint8_t readU8(RosMsgParser::Deserializer& d) {
  return d.deserialize(RosMsgParser::UINT8).extract<uint8_t>();
}

}  // namespace

// ---------------------------------------------------------------------------
// sensor_msgs/Image
//
// Wire layout (ROS2 shown; ROS1 prepends a uint32 seq inside the header):
//   header                  std_msgs/Header  (handled by readHeader())
//   height                  uint32
//   width                   uint32
//   encoding                string (e.g. "rgb8", "bgr8", "mono8", "mono16", "16UC1", …)
//   is_bigendian            uint8
//   step                    uint32
//   data                    uint8[height*step]
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::CanonicalObject> RosParser::parseImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    (void)readHeader();

    const uint32_t height = deserializer_->deserializeUInt32();
    const uint32_t width = deserializer_->deserializeUInt32();
    std::string encoding;
    deserializer_->deserializeString(encoding);
    const uint8_t is_be = readU8(*deserializer_);
    const uint32_t step = deserializer_->deserializeUInt32();
    const auto data_span = deserializer_->deserializeByteSequence();

    auto it = kRosImageEncodings().find(encoding);
    if (it == kRosImageEncodings().end()) {
      return PJ::unexpected(std::string("unsupported ROS encoding: ") + encoding);
    }
    const auto& enc = it->second;

    const size_t required = static_cast<size_t>(step) * height;
    if (data_span.size() < required) {
      return PJ::unexpected(std::string("Image data[] truncated"));
    }
    if (step < width * enc.bytes_per_pixel) {
      return PJ::unexpected(std::string("Image step smaller than width*bpp"));
    }

    return PJ::sdk::CanonicalObject{PJ::sdk::Image{
        .width = width,
        .height = height,
        .pixel_format = enc.format,
        .row_step = step,
        .is_bigendian = (is_be != 0),
        .pixels = PJ::Span<const uint8_t>(data_span.data(), required),
        .anchor = payload.anchor,
        .timestamp_ns = ts,
    }};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Image: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// sensor_msgs/CompressedImage
//
// Wire layout:
//   header                  std_msgs/Header
//   format                  string (e.g. "jpeg", "png", "16UC1; compressedDepth png")
//   data                    uint8[]   ← compressed bytes, plus an optional
//                                       12-byte compressedDepth mini-header
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::CanonicalObject> RosParser::parseCompressedImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    (void)readHeader();

    std::string format;
    deserializer_->deserializeString(format);
    const auto data_span = deserializer_->deserializeByteSequence();
    const uint8_t* src = data_span.data();
    const uint32_t data_len = static_cast<uint32_t>(data_span.size());

    PJ::sdk::CompressedImage::Format fmt = PJ::sdk::CompressedImage::Format::kUnknown;
    PJ::sdk::CompressedImage::Extras extras;
    size_t blob_offset = 0;
    uint32_t blob_size = data_len;

    if (format.find("compressedDepth") != std::string::npos) {
      if (data_len < 12) {
        return PJ::unexpected(std::string("compressedDepth data[] too short for header"));
      }
      // Mini-header: uint32 format (ignored), float depth_min, float depth_max.
      // This is inside a uint8[] body, so it is byte-packed — no CDR alignment.
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

    // Zero-copy: the bytes span is a slice of the payload; stripping the
    // 12-byte compressedDepth header is a pointer/length adjustment.
    PJ::sdk::CompressedImage out;
    out.format = fmt;
    out.bytes = PJ::Span<const uint8_t>(src + blob_offset, blob_size);
    out.anchor = payload.anchor;
    out.timestamp_ns = ts;
    out.extras = extras;
    return PJ::sdk::CanonicalObject{std::move(out)};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CompressedImage: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// sensor_msgs/PointCloud2
//
// Wire layout:
//   header                  std_msgs/Header
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

PJ::Expected<PJ::sdk::CanonicalObject> RosParser::parsePointCloud(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    (void)readHeader();

    const uint32_t height = deserializer_->deserializeUInt32();
    const uint32_t width = deserializer_->deserializeUInt32();

    const uint32_t fields_count = deserializer_->deserializeUInt32();
    if (fields_count > 1024) {
      return PJ::unexpected(std::string("PointCloud2: too many fields"));
    }
    std::vector<PJ::sdk::PointField> fields;
    fields.reserve(fields_count);
    for (uint32_t i = 0; i < fields_count; ++i) {
      std::string name;
      deserializer_->deserializeString(name);
      const uint32_t offset = deserializer_->deserializeUInt32();
      const uint8_t dt_raw = readU8(*deserializer_);
      const uint32_t count = deserializer_->deserializeUInt32();
      fields.push_back(
          PJ::sdk::PointField{
              .name = std::move(name),
              .offset = offset,
              .datatype = mapRosPointDatatype(dt_raw),
              .count = count,
          });
    }

    const uint8_t is_be = readU8(*deserializer_);
    const uint32_t point_step = deserializer_->deserializeUInt32();
    const uint32_t row_step = deserializer_->deserializeUInt32();
    const auto data_span = deserializer_->deserializeByteSequence();

    // is_dense follows data[]. deserializeByteSequence advanced the cursor
    // past the body, so the next byte read is is_dense itself.
    bool is_dense = true;
    if (deserializer_->bytesLeft() >= 1) {
      is_dense = (readU8(*deserializer_) != 0);
    }

    // Zero-copy: data_span is a slice of the payload. For a PointCloud2 with
    // a few MB of points this is the win — no per-message alloc/copy on the
    // hot path.
    return PJ::sdk::CanonicalObject{PJ::sdk::PointCloud{
        .width = width,
        .height = height,
        .point_step = point_step,
        .row_step = row_step,
        .is_bigendian = (is_be != 0),
        .is_dense = is_dense,
        .fields = std::move(fields),
        .data = PJ::Span<const uint8_t>(data_span.data(), data_span.size()),
        .anchor = payload.anchor,
        .timestamp_ns = ts,
    }};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PointCloud2: CDR read error: ") + e.what());
  }
}

}  // namespace ros_parser_detail
