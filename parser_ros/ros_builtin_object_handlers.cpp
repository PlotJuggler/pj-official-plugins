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

#include "pj_base/builtin/camera_info.hpp"
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
    HeaderData header = readHeader();

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
            .frame_id = std::move(header.frame_id),
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
    HeaderData header = readHeader();

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
            .frame_id = std::move(header.frame_id),
        }}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CompressedImage: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// sensor_msgs/CameraInfo
//
// Wire layout (ROS2 shown; ROS1 prepends a uint32 seq inside the header):
//   header                  std_msgs/Header   (handled by readHeader())
//   height                  uint32
//   width                   uint32
//   distortion_model        string
//   D                       float64[]    (sequence: uint32 count, then doubles)
//   K                       float64[9]   (fixed array — no count)
//   R                       float64[9]   (fixed array)
//   P                       float64[12]  (fixed array)
//   binning_x / binning_y / roi follow but are not part of sdk::CameraInfo, so
//   we stop after P (single-message positional decode; trailing fields are left
//   unread, like the other object handlers).
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseCameraInfo(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  // A float64[] D longer than this is corrupt — bound the reserve so a bad count
  // can't request a huge allocation (real distortion models use 4-8 coeffs).
  constexpr uint32_t kMaxDistortionCoeffs = 1024;
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    PJ::sdk::CameraInfo ci;
    ci.height = deserializer_->deserializeUInt32();
    ci.width = deserializer_->deserializeUInt32();
    deserializer_->deserializeString(ci.distortion_model);

    const uint32_t d_count = deserializer_->deserializeUInt32();  // D is a float64[] sequence
    if (d_count > kMaxDistortionCoeffs) {
      return PJ::unexpected(std::string("CameraInfo D[] exceeds sanity cap"));
    }
    ci.D.reserve(d_count);
    for (uint32_t i = 0; i < d_count; ++i) {
      ci.D.push_back(deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>());
    }
    for (double& k : ci.K) {  // K/R/P are fixed-size arrays — no length prefix
      k = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    }
    for (double& r : ci.R) {
      r = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    }
    for (double& p : ci.P) {
      p = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    }

    ci.frame_id = std::move(header.frame_id);
    ci.timestamp_ns = current_timestamp_;

    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(ci)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CameraInfo: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// foxglove_msgs/CompressedVideo
//
// Wire layout (ROS2 CDR):
//   timestamp               builtin_interfaces/Time  (sec int32, nanosec uint32)
//   frame_id                string
//   data                    uint8[]   ← compressed bitstream for ONE frame
//                                       (Annex-B for h264/h265), zero-copied
//   format                  string    ← lowercase codec id: "h264","h265","vp9","av1"
//
// Unlike the Image / PointCloud schemas, the first field is a BARE
// builtin_interfaces/Time, not a std_msgs/Header — so readHeader() must NOT be
// used (it would also consume a frame_id string that does not exist here, and
// the ROS1 seq branch). We read the two Time words directly instead.
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseCompressedVideo(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    // builtin_interfaces/Time timestamp — bare, not wrapped in a Header.
    const uint32_t sec = deserializer_->deserializeUInt32();
    const uint32_t nsec = deserializer_->deserializeUInt32();
    const int64_t embedded_ts_ns = static_cast<int64_t>(sec) * 1000000000LL + static_cast<int64_t>(nsec);
    if (use_embedded_timestamp_ && embedded_ts_ns > 0) {
      current_timestamp_ = embedded_ts_ns;
    }

    std::string frame_id;
    deserializer_->deserializeString(frame_id);
    const auto data_span = deserializer_->deserializeByteSequence();
    std::string format;
    deserializer_->deserializeString(format);

    // Zero-copy: data_span slices the payload buffer; payload.anchor keeps it alive.
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{PJ::sdk::VideoFrame{
            .timestamp_ns = current_timestamp_,
            .frame_id = std::move(frame_id),
            .format = std::move(format),
            .data = PJ::Span<const uint8_t>(data_span.data(), data_span.size()),
            .anchor = payload.anchor,
        }}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CompressedVideo: CDR read error: ") + e.what());
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
// sensor_msgs/LaserScan
//
// Wire layout (ROS2 shown; ROS1 prepends a uint32 seq inside the header):
//   header           std_msgs/Header  (handled by readHeader())
//   angle_min        float32
//   angle_max        float32   ← read + discard (derived from min + increment)
//   angle_increment  float32
//   time_increment   float32   ← read + discard
//   scan_time        float32   ← read + discard
//   range_min        float32
//   range_max        float32
//   ranges           float32[]
//   intensities      float32[]
//
// Eagerly projected to a canonical sdk::PointCloud through the shared
// pj_laser_scan projector: ray i lands at (r*cos(theta), r*sin(theta), 0) with
// theta = angle_min + i*angle_increment. Rays outside [range_min, range_max]
// or non-finite are dropped (the ROS contract says to discard them), so the
// output is unorganized and dense. NOT zero-copy by design: the wire carries
// polar ranges, so cartesian points are newly generated bytes owned by the
// cloud's anchor.
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseLaserScan(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    const float angle_min = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();
    (void)deserializer_->deserialize(RosMsgParser::FLOAT32);  // angle_max
    const float angle_increment = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();
    (void)deserializer_->deserialize(RosMsgParser::FLOAT32);  // time_increment
    (void)deserializer_->deserialize(RosMsgParser::FLOAT32);  // scan_time
    const float range_min = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();
    const float range_max = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();

    // float32[] sequences: validate the count against the remaining payload so
    // a corrupt length cannot request a huge allocation.
    const auto read_float_array = [this](std::vector<float>& out, const char* what) -> PJ::Status {
      const uint32_t count = deserializer_->deserializeUInt32();
      if (static_cast<size_t>(count) * sizeof(float) > deserializer_->bytesLeft()) {
        return PJ::unexpected(std::string("LaserScan ") + what + "[] longer than payload");
      }
      out.resize(count);
      for (uint32_t i = 0; i < count; ++i) {
        out[i] = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();
      }
      return PJ::okStatus();
    };

    std::vector<float>& ranges = laserscan_ranges_scratch_;
    std::vector<float>& intensities = laserscan_intensities_scratch_;
    ranges.clear();
    intensities.clear();
    if (auto status = read_float_array(ranges, "ranges"); !status) {
      return PJ::unexpected(std::move(status).error());
    }
    if (auto status = read_float_array(intensities, "intensities"); !status) {
      return PJ::unexpected(std::move(status).error());
    }

    PJ::laser_scan::ScanParams params;
    params.angle_min = static_cast<double>(angle_min);
    params.angle_increment = static_cast<double>(angle_increment);
    params.range_min = static_cast<double>(range_min);
    params.range_max = static_cast<double>(range_max);

    auto cloud = laser_projector_.project(
        params, PJ::Span<const float>(ranges.data(), ranges.size()),
        PJ::Span<const float>(intensities.data(), intensities.size()));
    cloud.frame_id = std::move(header.frame_id);
    cloud.timestamp_ns = current_timestamp_;

    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(cloud)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("LaserScan: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// foxglove_msgs/CompressedPointCloud
//
// Wire layout (ROS2 CDR):
//   timestamp               builtin_interfaces/Time  (sec int32, nanosec uint32)
//   frame_id                string
//   pose                    geometry_msgs/Pose       (position xyz f64, orientation xyzw f64)
//   data                    uint8[]   ← compressed blob (draco/cloudini/…), zero-copied
//   format                  string    ← lowercase codec id
//
// Like foxglove_msgs/CompressedVideo, the first field is a BARE
// builtin_interfaces/Time, not a std_msgs/Header — so readHeader() must NOT be
// used (it would consume the wrong fields). We read the two Time words directly.
//
// The pose's 7 doubles are read only to advance the cursor and then dropped:
// the canonical CompressedPointCloud has no pose; clouds are placed via TF on
// frame_id. A non-identity pose is silently ignored (no logger to warn on).
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseFoxgloveCompressedPointCloud(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    // builtin_interfaces/Time timestamp — bare, not wrapped in a Header.
    const uint32_t sec = deserializer_->deserializeUInt32();
    const uint32_t nsec = deserializer_->deserializeUInt32();
    const int64_t embedded_ts_ns = static_cast<int64_t>(sec) * 1000000000LL + static_cast<int64_t>(nsec);
    if (use_embedded_timestamp_ && embedded_ts_ns > 0) {
      current_timestamp_ = embedded_ts_ns;
    }

    std::string frame_id;
    deserializer_->deserializeString(frame_id);

    // geometry_msgs/Pose — 7 doubles (position xyz, orientation xyzw). Read to
    // advance the cursor, then drop: the canonical object carries no pose.
    for (int i = 0; i < 7; ++i) {
      (void)deserializer_->deserialize(RosMsgParser::FLOAT64);
    }

    const auto data_span = deserializer_->deserializeByteSequence();
    std::string format;
    deserializer_->deserializeString(format);

    // Zero-copy: data_span slices the payload buffer; payload.anchor keeps it alive.
    PJ::sdk::CompressedPointCloud cloud;
    cloud.timestamp_ns = current_timestamp_;
    cloud.frame_id = std::move(frame_id);
    cloud.format = std::move(format);
    cloud.data = PJ::Span<const uint8_t>(data_span.data(), data_span.size());
    cloud.anchor = payload.anchor;
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(cloud)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CompressedPointCloud: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// point_cloud_interfaces/CompressedPointCloud2
//
// Wire layout (point_cloud_transport canonical message):
//   header                  std_msgs/Header  (handled by readHeader())
//   height                  uint32
//   width                   uint32
//   fields                  sensor_msgs/PointField[]  (read + discard: the
//                                                       compressed blob is
//                                                       self-describing)
//     each PointField: string name, uint32 offset, uint8 datatype, uint32 count
//   is_bigendian            uint8
//   point_step              uint32
//   row_step                uint32
//   compressed_data         uint8[]   ← THE BLOB, zero-copied
//   is_dense                uint8
//   format                  string    ← LAST field; lowercase codec id
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseCompressedPointCloud2(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    auto header = readHeader();

    (void)deserializer_->deserializeUInt32();  // height
    (void)deserializer_->deserializeUInt32();  // width

    // fields[] — read and discard; the compressed blob is self-describing.
    const uint32_t fields_count = deserializer_->deserializeUInt32();
    if (fields_count > 1024) {
      return PJ::unexpected(std::string("CompressedPointCloud2: too many fields"));
    }
    for (uint32_t i = 0; i < fields_count; ++i) {
      std::string name;
      deserializer_->deserializeString(name);
      (void)deserializer_->deserializeUInt32();  // offset
      (void)readU8(*deserializer_);              // datatype
      (void)deserializer_->deserializeUInt32();  // count
    }

    (void)readU8(*deserializer_);              // is_bigendian
    (void)deserializer_->deserializeUInt32();  // point_step
    (void)deserializer_->deserializeUInt32();  // row_step
    const auto data_span = deserializer_->deserializeByteSequence();
    (void)readU8(*deserializer_);  // is_dense

    std::string format;
    deserializer_->deserializeString(format);

    // Zero-copy: data_span slices the payload buffer; payload.anchor keeps it alive.
    PJ::sdk::CompressedPointCloud cloud;
    cloud.timestamp_ns = current_timestamp_;
    cloud.frame_id = std::move(header.frame_id);
    cloud.format = std::move(format);
    cloud.data = PJ::Span<const uint8_t>(data_span.data(), data_span.size());
    cloud.anchor = payload.anchor;
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(cloud)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CompressedPointCloud2: CDR read error: ") + e.what());
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

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseFrameTransforms(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
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
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(transforms)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("TFMessage: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// geometry_msgs/TransformStamped — a single stamped transform on its own
// topic, surfaced as a one-element FrameTransforms so it feeds the same TF
// buffer as /tf. The scalar handler (handleTransformStamped) still runs.
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseTransformStampedObject(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    PJ::sdk::FrameTransforms transforms;
    transforms.transforms.push_back(readStampedTransform());
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(transforms)}};
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

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseOccupancyGrid(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    auto header = readHeader();

    // MapMetaData.map_load_time — read and discard; the grid uses the Header stamp.
    (void)deserializer_->deserialize(RosMsgParser::INT32);
    (void)deserializer_->deserialize(RosMsgParser::UINT32);

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
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(grid)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("OccupancyGrid: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// map_msgs/OccupancyGridUpdate
//
// Wire layout (ROS2 shown; ROS1 prepends a uint32 seq inside the header):
//   header   std_msgs/Header  (handled by readHeader())
//   x        int32   ← column offset of the patch top-left into the base grid
//   y        int32   ← row offset of the patch top-left
//   width    uint32  ← patch width in cells
//   height   uint32  ← patch height in cells
//   data     int8[width*height]   ← row-major patch cells
//
// The patch carries no origin/resolution; a stateful consumer places it at the
// base grid's origin + (x, y) * resolution (see occupancy_grid_update.hpp). The
// int8[] cells are byte-identical to uint8 on the wire, so the zero-copy span
// maps straight onto sdk::OccupancyGridUpdate::data.
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseOccupancyGridUpdate(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    auto header = readHeader();

    const int32_t x = deserializer_->deserialize(RosMsgParser::INT32).convert<int32_t>();
    const int32_t y = deserializer_->deserialize(RosMsgParser::INT32).convert<int32_t>();
    const uint32_t width = deserializer_->deserializeUInt32();
    const uint32_t height = deserializer_->deserializeUInt32();
    const auto data_span = deserializer_->deserializeByteSequence();

    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (data_span.size() < expected) {
      return PJ::unexpected(std::string("OccupancyGridUpdate data[] smaller than width*height"));
    }

    PJ::sdk::OccupancyGridUpdate update;
    update.timestamp_ns = current_timestamp_;
    update.frame_id = std::move(header.frame_id);
    update.x = x;
    update.y = y;
    update.width = width;
    update.height = height;
    // Zero-copy: data_span slices the payload buffer; the anchor keeps it alive.
    update.data = PJ::Span<const uint8_t>(data_span.data(), expected);
    update.anchor = payload.anchor;
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(update)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("OccupancyGridUpdate: CDR read error: ") + e.what());
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

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseRobotDescription(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
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
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(rd)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("RobotDescription: read error: ") + e.what());
  }
}

}  // namespace ros_parser_detail
