/**
 * @file ros_builtin_object_handlers.cpp
 * @brief Per-schema builtin-object decoders. One parse<X>() entry per
 *        schema returns an Expected<BuiltinObject> ready for ObjectStore
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
 *     reflected in the canonical fields (encoding string, row_step) so the
 *     consumer handles them — no parser-side conversion.
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

// Bytes per pixel for the raw ROS image encodings parser_ros consumes. Used
// only to validate that row_step >= width * bpp. Encoding strings are
// emitted into Image::encoding verbatim — the consumer routes by string.
const std::unordered_map<std::string, uint32_t>& kRosImageBytesPerPixel() {
  static const std::unordered_map<std::string, uint32_t> kMap = {
      {"rgb8", 3}, {"rgba8", 4}, {"bgr8", 3}, {"bgra8", 4}, {"mono8", 1}, {"mono16", 2}, {"16UC1", 2},
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

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    (void)readHeader();

    const uint32_t height = deserializer_->deserializeUInt32();
    const uint32_t width = deserializer_->deserializeUInt32();
    std::string encoding;
    deserializer_->deserializeString(encoding);
    const uint8_t is_be = readU8(*deserializer_);
    const uint32_t step = deserializer_->deserializeUInt32();
    const auto data_span = deserializer_->deserializeByteSequence();

    auto it = kRosImageBytesPerPixel().find(encoding);
    if (it == kRosImageBytesPerPixel().end()) {
      return PJ::unexpected(std::string("unsupported ROS encoding: ") + encoding);
    }
    const uint32_t bytes_per_pixel = it->second;

    const size_t required = static_cast<size_t>(step) * height;
    if (data_span.size() < required) {
      return PJ::unexpected(std::string("Image data[] truncated"));
    }
    if (step < width * bytes_per_pixel) {
      return PJ::unexpected(std::string("Image step smaller than width*bpp"));
    }

    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{PJ::sdk::Image{
            .width = width,
            .height = height,
            .encoding = encoding,
            .row_step = step,
            .is_bigendian = (is_be != 0),
            .data = PJ::Span<const uint8_t>(data_span.data(), required),
            .anchor = payload.anchor,
            .compressed_depth_min = std::nullopt,
            .compressed_depth_max = std::nullopt,
            .timestamp_ns = current_timestamp_,
        }}};
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

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseCompressedImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    (void)readHeader();

    std::string format;
    deserializer_->deserializeString(format);
    const auto data_span = deserializer_->deserializeByteSequence();
    const uint8_t* src = data_span.data();
    const uint32_t data_len = static_cast<uint32_t>(data_span.size());

    std::string out_encoding;
    std::optional<float> depth_min;
    std::optional<float> depth_max;
    size_t blob_offset = 0;
    uint32_t blob_size = data_len;

    if (format.find("compressedDepth") != std::string::npos) {
      if (data_len < 12) {
        return PJ::unexpected(std::string("compressedDepth data[] too short for header"));
      }
      // Mini-header: uint32 format (ignored), float depth_min, float depth_max.
      // This is inside a uint8[] body, so it is byte-packed — no CDR alignment.
      float dmin = 0.0f;
      float dmax = 0.0f;
      std::memcpy(&dmin, src + 4, sizeof(float));
      std::memcpy(&dmax, src + 8, sizeof(float));
      depth_min = dmin;
      depth_max = dmax;
      out_encoding = "compressedDepth";  // PNG payload + depth quantization range.
      blob_offset = 12;
      blob_size = data_len - 12;
    } else if (format.find("jpeg") != std::string::npos) {
      out_encoding = "jpeg";
    } else if (format == "png") {
      out_encoding = "png";
    } else {
      return PJ::unexpected(std::string("unsupported CompressedImage format: ") + format);
    }

    // Zero-copy: the bytes span is a slice of the payload; stripping the
    // 12-byte compressedDepth header is a pointer/length adjustment.
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{PJ::sdk::Image{
            .width = 0,
            .height = 0,
            .encoding = std::move(out_encoding),
            .row_step = 0,
            .is_bigendian = false,
            .data = PJ::Span<const uint8_t>(src + blob_offset, blob_size),
            .anchor = payload.anchor,
            .compressed_depth_min = depth_min,
            .compressed_depth_max = depth_max,
            .timestamp_ns = current_timestamp_,
        }}};
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

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePointCloud(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    auto header = readHeader();

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
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{PJ::sdk::PointCloud{
            .width = width,
            .height = height,
            .point_step = point_step,
            .row_step = row_step,
            .is_bigendian = (is_be != 0),
            .is_dense = is_dense,
            .frame_id = std::move(header.frame_id),
            .fields = std::move(fields),
            .data = PJ::Span<const uint8_t>(data_span.data(), data_span.size()),
            .anchor = payload.anchor,
            .timestamp_ns = current_timestamp_,
        }}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PointCloud2: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// tf2_msgs/TFMessage
//
// Wire layout:
//   transforms              geometry_msgs/TransformStamped[]  (uint32 count + N)
//     each TransformStamped:
//       header              std_msgs/Header  (sec, nanosec, frame_id = parent)
//       child_frame_id      string
//       transform           geometry_msgs/Transform
//         translation       Vector3     (x, y, z   : float64)
//         rotation          Quaternion  (x, y, z, w: float64)
//
// Emitted as a canonical sdk::FrameTransforms (owned — no byte blob). Each
// FrameTransform carries its OWN Header.stamp: that per-sample time is what the
// 3D scene's TF buffer needs for zero-order-hold scrub lookups, independent of
// the message receive time. The scalar handler (handleTFMessage) still runs in
// parallel for users who want to plot the transforms as time series.
// ---------------------------------------------------------------------------

PJ::sdk::FrameTransform RosParser::readStampedTransform() {
  HeaderData header = readHeader();
  std::string child_frame_id;
  deserializer_->deserializeString(child_frame_id);

  const double tx = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double ty = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double tz = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double qx = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double qy = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double qz = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double qw = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();

  PJ::sdk::FrameTransform tf;
  tf.timestamp =
      static_cast<PJ::Timestamp>(static_cast<int64_t>(header.sec) * 1000000000LL + static_cast<int64_t>(header.nsec));
  tf.parent_frame_id = std::move(header.frame_id);
  tf.child_frame_id = std::move(child_frame_id);
  tf.translation = {.x = tx, .y = ty, .z = tz};
  tf.rotation = {.x = qx, .y = qy, .z = qz, .w = qw};
  return tf;
}

PJ::Expected<PJ::sdk::BuiltinObject> RosParser::parseFrameTransforms(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    const uint32_t transform_count = deserializer_->deserializeUInt32();
    PJ::sdk::FrameTransforms transforms;
    transforms.transforms.reserve(transform_count);
    for (uint32_t i = 0; i < transform_count; ++i) {
      transforms.transforms.push_back(readStampedTransform());
    }
    return PJ::sdk::BuiltinObject{std::move(transforms)};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("TFMessage: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// geometry_msgs/TransformStamped — a single stamped transform on its own
// topic, surfaced as a one-element FrameTransforms so it feeds the same TF
// buffer as /tf. The scalar handler (handleTransformStamped) still runs.
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::BuiltinObject> RosParser::parseTransformStampedObject(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    PJ::sdk::FrameTransforms transforms;
    transforms.transforms.push_back(readStampedTransform());
    return PJ::sdk::BuiltinObject{std::move(transforms)};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("TransformStamped: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// nav_msgs/OccupancyGrid
//
// Wire layout:
//   header        std_msgs/Header   (sec, nanosec, frame_id)
//   info          nav_msgs/MapMetaData
//     map_load_time  builtin_interfaces/Time  (sec, nanosec)   [read + discard]
//     resolution     float32
//     width          uint32
//     height         uint32
//     origin         geometry_msgs/Pose  (position xyz f64, orientation xyzw f64)
//   data          int8[]            (uint32 count + bytes)
//
// Byte-backed: the cell bytes are zero-copied as a Span over the payload,
// pinned by payload.anchor (same pattern as PointCloud2 / Image).
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::BuiltinObject> RosParser::parseOccupancyGrid(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    auto header = readHeader();

    // MapMetaData.map_load_time — read and discard; the grid uses the Header stamp.
    deserializer_->deserialize(RosMsgParser::INT32);
    deserializer_->deserialize(RosMsgParser::UINT32);

    const float resolution = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();
    const uint32_t width = deserializer_->deserializeUInt32();
    const uint32_t height = deserializer_->deserializeUInt32();

    const double px = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    const double py = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    const double pz = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    const double ox = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    const double oy = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    const double oz = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    const double ow = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();

    const auto data_span = deserializer_->deserializeByteSequence();

    PJ::sdk::OccupancyGrid grid;
    grid.timestamp_ns = current_timestamp_;
    grid.frame_id = std::move(header.frame_id);
    grid.origin.position = {.x = px, .y = py, .z = pz};
    grid.origin.orientation = {.x = ox, .y = oy, .z = oz, .w = ow};
    grid.resolution = static_cast<double>(resolution);
    grid.width = width;
    grid.height = height;
    grid.data = PJ::Span<const uint8_t>(data_span.data(), data_span.size());
    grid.anchor = payload.anchor;
    return PJ::sdk::BuiltinObject{std::move(grid)};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("OccupancyGrid: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// std_msgs/String on a robot_description topic -> sdk::RobotDescription
//
// Dispatched in bindSchema by topic name (a generic String stays generic).
// The body is one string (the URDF/SDF/MJCF source); we carry it verbatim
// plus a best-effort format hint sniffed from the root element. Downstream
// consumers do the format-specific parsing.
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::BuiltinObject> RosParser::parseRobotDescription(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    std::string text;
    deserializer_->deserializeString(text);

    PJ::sdk::RobotDescription rd;
    rd.timestamp_ns = current_timestamp_;
    rd.topic = topic_name_;
    if (text.find("<robot") != std::string::npos) {
      rd.format = "urdf";
    } else if (text.find("<sdf") != std::string::npos) {
      rd.format = "sdf";
    } else if (text.find("<mujoco") != std::string::npos) {
      rd.format = "mjcf";
    }
    rd.text = std::move(text);
    return PJ::sdk::BuiltinObject{std::move(rd)};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("RobotDescription: read error: ") + e.what());
  }
}

}  // namespace ros_parser_detail
