/**
 * @file ros_builtin_object_handlers.cpp
 * @brief Per-schema builtin-object decoders: each parse<X>() sets up the deserializer, walks the
 *        body and returns the object for ObjectStore ingestion.
 *
 * The parser's RosMsgParser::Deserializer (chosen in bindSchema by use_ros1_) serves both wires:
 * NanoCDR_Deserializer applies XCDR1 origin-relative alignment and honours the encapsulation
 * header's endianness flag, ROS_Deserializer reads packed little-endian ROS 1. readHeader()
 * branches on isROS2() for the ROS 1 seq field.
 *
 * Zero-copy: the bulk byte array of each schema (Image::data, PointCloud2::data,
 * CompressedImage::data) is read with deserializeByteSequence(), a span over the host's payload
 * buffer, and `payload.anchor` is propagated into the result to keep it alive. BGR ordering and
 * per-row padding are left to the consumer through the canonical encoding string and row_step;
 * nothing is converted here.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <pj_grid_map/grid_map_transcoder.hpp>
#include <pj_pointcloud_color/pointcloud_color.hpp>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/log.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

namespace {

// Bytes per pixel of the raw encodings, only to validate row_step >= width * bpp; Image::encoding
// keeps the string verbatim. Bayer CFA encodings (bayer_*8) are one mosaic sample per pixel;
// demosaicing happens downstream in the viewer.
const std::unordered_map<std::string, uint32_t>& kRosImageBytesPerPixel() {
  static const std::unordered_map<std::string, uint32_t> kMap = {
      {"rgb8", 3},  {"rgba8", 4}, {"bgr8", 3},        {"bgra8", 4},       {"mono8", 1},       {"mono16", 2},
      {"16UC1", 2}, {"32FC1", 4}, {"bayer_rggb8", 1}, {"bayer_grbg8", 1}, {"bayer_gbrg8", 1}, {"bayer_bggr8", 1},
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

/// Bounds a wire-declared element count by the bytes left at the cursor, so a
/// corrupt length prefix cannot request a huge allocation before the read fails.
void requireAvailable(const RosMsgParser::Deserializer& d, uint64_t count, size_t min_bytes_each, const char* what) {
  if (count * min_bytes_each > d.bytesLeft()) {
    throw std::runtime_error(std::string(what) + " count exceeds message payload (truncated or corrupt message)");
  }
}

}  // namespace

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
    const auto data_span = readByteSequence();

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

// `format` is e.g. "jpeg", "bgr8; png compressed bgr8", "16UC1; compressedDepth png" or just "16UC1";
// a compressedDepth data[] may start with a 12-byte mini-header.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseCompressedImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    std::string format;
    deserializer_->deserializeString(format);
    const auto data_span = readByteSequence();
    const uint8_t* src = data_span.data();
    const uint32_t data_len = static_cast<uint32_t>(data_span.size());

    std::string out_encoding;
    std::optional<float> depth_min;
    std::optional<float> depth_max;
    size_t blob_offset = 0;
    uint32_t blob_size = data_len;

    static constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr uint8_t kJpegSoi[3] = {0xFF, 0xD8, 0xFF};
    auto data_starts_with = [&](const auto& magic) {
      return data_len >= sizeof(magic) && std::memcmp(src, magic, sizeof(magic)) == 0;
    };

    if (format.find("compressedDepth") != std::string::npos) {
      out_encoding = "compressedDepth";  // PNG/RVL payload (+ optional quantization range).
      // compressedDepth normally prefixes the PNG/RVL blob with a 12-byte ConfigHeader (uint32
      // format, float depthQuantA, float depthQuantB), but some recorders (seen in RealSense bags)
      // emit a BARE PNG, and stripping 12 bytes there corrupts it. A leading PNG signature
      // therefore means headerless: pass the blob through untouched.
      if (data_starts_with(kPngSignature)) {
        // No ConfigHeader -> no quantization range. 16UC1 PNG depth is raw
        // millimetres, which the consumer reads directly; depth_min/max stay unset.
        blob_offset = 0;
        blob_size = data_len;
      } else {
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
        blob_offset = 12;
        blob_size = data_len - 12;
      }
    } else if (format.find("jpeg") != std::string::npos || data_starts_with(kJpegSoi)) {
      // The magic-byte checks cover recorders whose format names only the raw
      // encoding (e.g. "16UC1"), not the codec.
      out_encoding = "jpeg";
    } else if (format.find("png") != std::string::npos || data_starts_with(kPngSignature)) {
      // A 16UC1 PNG is raw millimetres: route it down the depth path as
      // headerless compressedDepth (no quantization range).
      out_encoding = format.rfind("16UC1", 0) == 0 ? "compressedDepth" : "png";
    } else {
      return PJ::unexpected(std::string("unsupported CompressedImage format: ") + format);
    }

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

// binning_x / binning_y / roi follow P but are not part of sdk::CameraInfo; the trailing fields are
// left unread, like the other object handlers.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseCameraInfo(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  // A longer float64[] D is corrupt (real distortion models use 4-8 coeffs); bounds the reserve.
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

// Leads with a BARE builtin_interfaces/Time, not a std_msgs/Header: readHeader() would also read a
// frame_id that is not there (and the ROS 1 seq). `data` is one frame's bitstream (Annex-B for
// h264/h265), `format` a lowercase codec id ("h264", "h265", "vp9", "av1").
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseCompressedVideo(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    // readBareTime() reads `sec` as int32 and adopts the embedded stamp.
    (void)readBareTime();

    std::string frame_id;
    deserializer_->deserializeString(frame_id);
    const auto data_span = readByteSequence();
    std::string format;
    deserializer_->deserializeString(format);

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
    const auto data_span = readByteSequence();

    // is_dense follows data[]: the cursor is already on it.
    bool is_dense = true;
    if (deserializer_->bytesLeft() >= 1) {
      is_dense = (readU8(*deserializer_) != 0);
    }

    PJ::sdk::PointCloud cloud{
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
    };
    // Normalizes colour to the canonical packed "rgba" field, so the host renders one per-point
    // colour instead of offering each channel as a colormap source. A PCL packed rgb/rgba
    // (0x00RRGGBB) is repacked into R,G,B,A order in a fresh owned buffer (zero-copy is given up
    // only for colour clouds); separate red/green/blue/alpha channels collapse zero-copy; plain
    // XYZI clouds are untouched.
    pj::pointcloud_color::normalizeCanonicalColor(cloud);

    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(cloud)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PointCloud2: CDR read error: ") + e.what());
  }
}

// Header series plus /num_points = len(data) / point_step, the points ACTUALLY present, deliberately
// NOT width * height: those agree only for a well-formed dense cloud, and a truncated or partly filled
// message (or a producer leaving the sensor's nominal grid in width/height) makes width * height a
// claim rather than a measurement. The series is worth plotting because it reveals dropped points.
// Only data[]'s length prefix is read; the bulk bytes are never touched.
PJ::Status RosParser::parsePointCloud2Scalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);
    emitHeader(readHeader());

    (void)deserializer_->deserializeUInt32();  // height
    (void)deserializer_->deserializeUInt32();  // width

    // fields[]: read and discarded, the cursor only has to reach point_step.
    const uint32_t fields_count = deserializer_->deserializeUInt32();
    if (fields_count > 1024) {
      return PJ::unexpected(std::string("PointCloud2 scalars: too many fields"));
    }
    for (uint32_t i = 0; i < fields_count; ++i) {
      std::string name;
      deserializer_->deserializeString(name);
      (void)deserializer_->deserializeUInt32();  // offset
      (void)readU8(*deserializer_);              // datatype
      (void)deserializer_->deserializeUInt32();  // count
    }

    (void)readU8(*deserializer_);  // is_bigendian
    const uint32_t point_step = deserializer_->deserializeUInt32();
    (void)deserializer_->deserializeUInt32();  // row_step

    const uint32_t data_size = deserializer_->deserializeUInt32();
    if (data_size > deserializer_->bytesLeft()) {
      // Truncated or corrupt; the same verdict readByteSequence() reaches on the object route.
      return PJ::unexpected(std::string("PointCloud2 scalars: data[] length exceeds message payload"));
    }
    // point_step == 0 is malformed: omit the column instead of dividing by zero or filing a
    // fabricated count.
    if (point_step > 0) {
      addField("/num_points", PJ::sdk::ValueRef{static_cast<uint64_t>(data_size) / point_step});
    }
    return PJ::okStatus();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PointCloud2 scalars: CDR read error: ") + e.what());
  }
}

// Eagerly projected through pj_laser_scan: ray i lands at (r*cos(theta), r*sin(theta), 0) with
// theta = angle_min + i*angle_increment. Rays outside [range_min, range_max] or non-finite are dropped
// (the ROS contract says to discard them), so the cloud is unorganized and dense. Not zero-copy: the
// wire carries polar ranges, so the cartesian points are newly generated bytes owned by the cloud's anchor.
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

    // Validates each float32[] count against the remaining payload before allocating.
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

// Leads with a BARE builtin_interfaces/Time (see parseCompressedVideo). The pose's 7 doubles are read
// only to advance the cursor: the canonical CompressedPointCloud has no pose (clouds are placed via TF
// on frame_id), so a non-identity pose is silently ignored.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseFoxgloveCompressedPointCloud(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    // readBareTime() reads `sec` as int32 and adopts the embedded stamp.
    (void)readBareTime();

    std::string frame_id;
    deserializer_->deserializeString(frame_id);

    // geometry_msgs/Pose (7 doubles): read to advance the cursor, then dropped.
    for (int i = 0; i < 7; ++i) {
      (void)deserializer_->deserialize(RosMsgParser::FLOAT64);
    }

    const auto data_span = readByteSequence();
    std::string format;
    deserializer_->deserializeString(format);

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

// Slim scalar companion for foxglove_msgs/CompressedPointCloud. The wire head is a BARE
// builtin_interfaces/Time then frame_id, not a std_msgs/Header (readHeader()'s ROS 1 branch would
// consume `seq` first and shift every read), so it emits the flat "/timestamp" + "/frame_id" pair
// instead of /header/*. The opaque compressed blob has no point count, so nothing else is emitted.
PJ::Status RosParser::parseFoxgloveCompressedPointCloudScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);

    // The embedded stamp becomes the record time only when the user asked for it, as with
    // readHeader() (registerBoundSchemaHandler reads current_timestamp_ back out).
    const int64_t stamp_ns = readBareTime();
    addField("/timestamp", nanosecondsToSeconds(stamp_ns));

    std::string frame_id;
    deserializer_->deserializeString(frame_id);
    addStringField("/frame_id", frame_id);

    return PJ::okStatus();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CompressedPointCloud scalars: CDR read error: ") + e.what());
  }
}

// Layout: header, height, width, fields[] (read and discarded: the blob is self-describing),
// is_bigendian, point_step, row_step, compressed_data (the blob), is_dense, then format (lowercase
// codec id) LAST.
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
    const auto data_span = readByteSequence();
    (void)readU8(*deserializer_);  // is_dense

    std::string format;
    deserializer_->deserializeString(format);

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

// Emitted as an owned sdk::FrameTransforms. The 3D scene's TF buffer keys each edge by
// FrameTransform::timestamp and scrubs it with a zero-order hold at the playhead, which lives on the
// MCAP message clock (log / publish time); readStampedTransform() resolves the per-sample timestamp
// onto that clock (three cases there). handleTFMessage still runs for plotting the transforms as time
// series and always keeps the raw Header.stamp.
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

  const int64_t header_stamp_ns = static_cast<int64_t>(header.sec) * 1000000000LL + static_cast<int64_t>(header.nsec);

  PJ::sdk::FrameTransform tf;
  // Timestamp resolution, so the TF buffer lands on the clock the 3D scene scrubs on:
  //   1. "use embedded timestamp" ON -> the payload Header.stamp, as for every other message type.
  //   2. OFF + Header.stamp == 0 -> keep 0: a static transform (/tf_static latched with an unset
  //      stamp) whose single sample must hold for every query time via the zero-order hold.
  //   3. OFF + Header.stamp != 0 -> current_timestamp_, the MCAP-resolved message time. A bag
  //      recorded across unsynchronized clocks carries a Header.stamp on another clock (offset AND
  //      rate) than the playhead; keying by it would drift transforms out of sync with point clouds.
  if (use_embedded_timestamp_ || header_stamp_ns == 0) {
    tf.timestamp_ns = static_cast<PJ::Timestamp>(header_stamp_ns);
  } else {
    tf.timestamp_ns = current_timestamp_;
  }
  tf.parent_frame_id = std::move(header.frame_id);
  tf.child_frame_id = std::move(child_frame_id);
  tf.translation = {.x = tx, .y = ty, .z = tz};
  tf.rotation = {.x = qx, .y = qy, .z = qz, .w = qw};
  return tf;
}

// geometry_msgs/Pose: position xyz then orientation xyzw, 7 float64, no header.
PJ::sdk::Pose RosParser::readPose() {
  const double px = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double py = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double pz = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double ox = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double oy = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double oz = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  const double ow = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  return PJ::sdk::Pose{
      .position = {.x = px, .y = py, .z = pz},
      .orientation = {.x = ox, .y = oy, .z = oz, .w = ow},
  };
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

// A single stamped transform as a one-element FrameTransforms, feeding the same TF buffer as /tf;
// handleTransformStamped still runs.
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

// Emitted as an owned sdk::PosesInFrame. The canonical type stores ONLY poses: rendering style
// (arrow vs triad, size, color) is the viewer's choice.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePoseArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    const uint32_t pose_count = deserializer_->deserializeUInt32();
    // Corrupt-count guard: 7 float64 = 56 bytes is the minimum wire size per
    // pose; XCDR1 alignment padding only makes valid messages larger, so this
    // lower bound never rejects a valid message. The surrounding try/catch is
    // the authoritative failure mode for truncation.
    constexpr size_t kPoseWireBytes = 7 * sizeof(double);
    if (static_cast<size_t>(pose_count) * kPoseWireBytes > deserializer_->bytesLeft()) {
      return PJ::unexpected(std::string("PoseArray: pose count exceeds payload size"));
    }

    PJ::sdk::PosesInFrame result;
    result.timestamp_ns = current_timestamp_;
    result.frame_id = std::move(header.frame_id);
    result.poses.reserve(pose_count);
    for (uint32_t i = 0; i < pose_count; ++i) {
      result.poses.push_back(readPose());
    }
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PoseArray: CDR read error: ") + e.what());
  }
}

// Leads with a BARE builtin_interfaces/Time (see parseCompressedVideo).
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseFoxglovePosesInFrame(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    // readBareTime() reads `sec` as int32 and adopts the embedded stamp.
    (void)readBareTime();

    std::string frame_id;
    deserializer_->deserializeString(frame_id);

    const uint32_t pose_count = deserializer_->deserializeUInt32();
    constexpr size_t kPoseWireBytes = 7 * sizeof(double);
    if (static_cast<size_t>(pose_count) * kPoseWireBytes > deserializer_->bytesLeft()) {
      return PJ::unexpected(std::string("PosesInFrame: pose count exceeds payload size"));
    }

    PJ::sdk::PosesInFrame result;
    result.timestamp_ns = current_timestamp_;
    result.frame_id = std::move(frame_id);
    result.poses.reserve(pose_count);
    for (uint32_t i = 0; i < pose_count; ++i) {
      result.poses.push_back(readPose());
    }
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PosesInFrame: CDR read error: ") + e.what());
  }
}

// rcl_interfaces/Log (ROS 2) and rosgraph_msgs/Log (ROS 1) -> sdk::Log; the generic scalar flatten
// still runs so `level` etc. stay plottable.
//
// ROS 2: Time stamp; uint8 level; string name, msg, file, function; uint32 line.
// ROS 1: Header; byte level; the same strings; uint32 line; string[] topics (last and unused, so the
// read stops after `line`). Level constants differ: ROS 2 10/20/30/40/50, ROS 1 1/2/4/8/16.
namespace {

PJ::sdk::Log::Level mapRosLogLevel(uint8_t level, bool ros2) {
  using Level = PJ::sdk::Log::Level;
  if (ros2) {
    switch (level) {
      case 10:
        return Level::kDebug;
      case 20:
        return Level::kInfo;
      case 30:
        return Level::kWarning;
      case 40:
        return Level::kError;
      case 50:
        return Level::kFatal;
      default:
        return Level::kUnknown;
    }
  }
  switch (level) {
    case 1:
      return Level::kDebug;
    case 2:
      return Level::kInfo;
    case 4:
      return Level::kWarning;
    case 8:
      return Level::kError;
    case 16:
      return Level::kFatal;
    default:
      return Level::kUnknown;
  }
}

}  // namespace

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseLogObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    const bool ros2 = deserializer_->isROS2();
    int64_t stamp_ns = 0;
    if (ros2) {
      stamp_ns = readBareTime();
    } else {
      const HeaderData header = readHeader();
      stamp_ns = header.sec * 1000000000LL + static_cast<int64_t>(header.nsec);
    }

    PJ::sdk::Log result;
    result.timestamp_ns = stamp_ns != 0 ? stamp_ns : static_cast<int64_t>(ts);
    result.level = mapRosLogLevel(readU8(*deserializer_), ros2);
    deserializer_->deserializeString(result.source);
    deserializer_->deserializeString(result.message);
    // The source location (file, function, line) is not part of the canonical Log; it is read only so a
    // truncated message is rejected instead of accepted.
    std::string skipped;
    deserializer_->deserializeString(skipped);
    deserializer_->deserializeString(skipped);
    (void)deserializer_->deserializeUInt32();  // line
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Log: CDR read error: ") + e.what());
  }
}

// Other log-equivalent messages -> sdk::Log (scalar route: generic flatten).
//
// Wire layouts:
//   foxglove_msgs/Log:      builtin_interfaces/Time (ROS 1: time) timestamp; uint8 level
//                           (0..5 = Foxglove Log.Level, same as sdk::Log::Level);
//                           string message; string name; string file; uint32 line
//   mavros_msgs/StatusText: std_msgs/Header header; uint8 severity; string text
//   px4_msgs/LogMessage:    uint64 timestamp; uint8 severity; char[127] text
//   px4_msgs/MavlinkLog:    uint64 timestamp; char[127] text; uint8 severity
// StatusText and the PX4 messages use syslog severities 0..7 (EMERGENCY..DEBUG).
// The PX4 timestamp is microseconds since boot, not wall time, so the record
// timestamp is used instead.
namespace {

PJ::sdk::Log::Level mapSyslogSeverity(uint8_t severity) {
  using Level = PJ::sdk::Log::Level;
  switch (severity) {
    case 0:  // EMERGENCY
    case 1:  // ALERT
    case 2:  // CRITICAL
      return Level::kFatal;
    case 3:
      return Level::kError;
    case 4:
      return Level::kWarning;
    case 5:  // NOTICE
    case 6:  // INFO
      return Level::kInfo;
    case 7:
      return Level::kDebug;
    default:
      return Level::kUnknown;
  }
}

/// Reads a fixed char[N] array and returns its text up to the first NUL.
std::string readFixedText(RosMsgParser::Deserializer& d, size_t length) {
  if (d.bytesLeft() < length) {
    throw std::runtime_error("text array exceeds message payload");
  }
  const auto* begin = reinterpret_cast<const char*>(d.getCurrentPtr());
  std::string text(begin, std::find(begin, begin + length, '\0'));
  d.jump(length);
  return text;
}

}  // namespace

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseFoxgloveLogObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    const int64_t stamp_ns = readBareTime();
    PJ::sdk::Log result;
    result.timestamp_ns = stamp_ns != 0 ? stamp_ns : static_cast<int64_t>(ts);
    const uint8_t level = readU8(*deserializer_);
    result.level = level <= 5 ? static_cast<PJ::sdk::Log::Level>(level) : PJ::sdk::Log::Level::kUnknown;
    deserializer_->deserializeString(result.message);
    deserializer_->deserializeString(result.source);
    std::string file;  // read only so a truncated message is rejected
    deserializer_->deserializeString(file);
    (void)deserializer_->deserializeUInt32();  // line
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Log: CDR read error: ") + e.what());
  }
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseStatusTextLogObject(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    const HeaderData header = readHeader();
    const int64_t stamp_ns = header.sec * 1000000000LL + static_cast<int64_t>(header.nsec);
    PJ::sdk::Log result;
    result.timestamp_ns = stamp_ns != 0 ? stamp_ns : static_cast<int64_t>(ts);
    result.level = mapSyslogSeverity(readU8(*deserializer_));
    deserializer_->deserializeString(result.message);
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("StatusText: CDR read error: ") + e.what());
  }
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePx4LogMessageObject(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  return parsePx4Log(ts, payload, /*severity_first=*/true);
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePx4MavlinkLogObject(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  return parsePx4Log(ts, payload, /*severity_first=*/false);
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePx4Log(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload, bool severity_first) {
  constexpr size_t kTextLength = 127;
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    (void)deserializer_->deserialize(RosMsgParser::UINT64);  // timestamp: microseconds since boot
    PJ::sdk::Log result;
    result.timestamp_ns = ts;
    if (severity_first) {
      result.level = mapSyslogSeverity(readU8(*deserializer_));
      result.message = readFixedText(*deserializer_, kTextLength);
    } else {
      result.message = readFixedText(*deserializer_, kTextLength);
      result.level = mapSyslogSeverity(readU8(*deserializer_));
    }
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PX4 log: CDR read error: ") + e.what());
  }
}

// A one-element PosesInFrame feeding the same 3D pose view as PoseArray; handlePoseStamped still runs
// for per-axis plotting.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePoseStampedObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    PJ::sdk::PosesInFrame result;
    result.timestamp_ns = current_timestamp_;
    result.frame_id = std::move(header.frame_id);
    result.poses.push_back(readPose());
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PoseStamped: CDR read error: ") + e.what());
  }
}

// nav_msgs/Odometry: the pose of child_frame_id in the header frame, as a one-element PosesInFrame;
// handleOdometry still runs for per-axis plotting of pose, twist and covariances.
// Only Header + child_frame_id + Pose are consumed (twist and covariance are left unread). The object
// adopts the HEADER frame_id (the reference frame), not child_frame_id.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseOdometryObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    std::string child_frame_id;
    deserializer_->deserializeString(child_frame_id);  // read + dropped (object uses the header frame)

    PJ::sdk::PosesInFrame result;
    result.timestamp_ns = current_timestamp_;
    result.frame_id = std::move(header.frame_id);
    result.poses.push_back(readPose());
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Odometry: CDR read error: ") + e.what());
  }
}

// A PosesInFrame is an array of poses in ONE frame at ONE instant, so the object adopts the PATH
// header's frame_id + stamp. Each PoseStamped's own Header is read and dropped (readHeader's
// current_timestamp_ side effect is overwritten by the path stamp captured up front). Per-pose
// stamps/frames stay available via the generic scalar route.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePath(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData path_header = readHeader();
    // Capture the path-level time before the per-pose readHeader() calls below
    // overwrite current_timestamp_ with each pose's own stamp.
    const PJ::Timestamp object_ts = current_timestamp_;

    const uint32_t pose_count = deserializer_->deserializeUInt32();
    // Corrupt-count guard: each PoseStamped is at minimum a 12-byte Header
    // (sec + nanosec + empty frame_id) plus a 56-byte Pose. CDR alignment only
    // adds bytes, so this lower bound never rejects a valid message.
    constexpr size_t kPoseStampedMinBytes = 12 + 7 * sizeof(double);
    if (static_cast<size_t>(pose_count) * kPoseStampedMinBytes > deserializer_->bytesLeft()) {
      return PJ::unexpected(std::string("Path: pose count exceeds payload size"));
    }

    PJ::sdk::PosesInFrame result;
    result.timestamp_ns = object_ts;
    result.frame_id = std::move(path_header.frame_id);
    result.poses.reserve(pose_count);
    for (uint32_t i = 0; i < pose_count; ++i) {
      readHeader();  // per-pose Header — read and dropped
      result.poses.push_back(readPose());
    }
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{object_ts} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Path: CDR read error: ") + e.what());
  }
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePoseWithCovarianceStampedObject(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    PJ::sdk::PosesInFrame result;
    result.timestamp_ns = current_timestamp_;
    result.frame_id = std::move(header.frame_id);
    result.poses.push_back(readPose());  // the float64[36] covariance after it is left unread
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PoseWithCovarianceStamped: CDR read error: ") + e.what());
  }
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseParticleCloud(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    const uint32_t particle_count = deserializer_->deserializeUInt32();
    // Corrupt-count guard: a Particle is a 56-byte Pose plus a float64 weight.
    constexpr size_t kParticleWireBytes = 8 * sizeof(double);
    if (static_cast<size_t>(particle_count) * kParticleWireBytes > deserializer_->bytesLeft()) {
      return PJ::unexpected(std::string("ParticleCloud: particle count exceeds payload size"));
    }

    PJ::sdk::PosesInFrame result;
    result.timestamp_ns = current_timestamp_;
    result.frame_id = std::move(header.frame_id);
    result.poses.reserve(particle_count);
    for (uint32_t i = 0; i < particle_count; ++i) {
      result.poses.push_back(readPose());
      (void)deserializer_->deserialize(RosMsgParser::FLOAT64);  // weight
    }
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(result)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("ParticleCloud: CDR read error: ") + e.what());
  }
}

// Frame-locked because a footprint is usually published rarely (or once) in the robot's own frame
// and must move with it; for a polygon in a fixed frame locking changes nothing. Each message
// replaces the previous one through the fixed entity id.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parsePolygonStamped(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    HeaderData header = readHeader();

    const uint32_t point_count = deserializer_->deserializeUInt32();
    constexpr size_t kPoint32WireBytes = 3 * sizeof(float);
    if (static_cast<size_t>(point_count) * kPoint32WireBytes > deserializer_->bytesLeft()) {
      return PJ::unexpected(std::string("PolygonStamped: point count exceeds payload size"));
    }

    PJ::sdk::LinePrimitive line;
    line.type = PJ::sdk::LineType::kLineLoop;
    line.pose.orientation.w = 1.0;
    line.thickness = 2.0;
    line.scale_invariant = true;
    line.color = {.r = 25, .g = 255, .b = 0, .a = 255};  // RViz's Polygon display default
    line.points.reserve(point_count);
    for (uint32_t i = 0; i < point_count; ++i) {
      const double x = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<double>();
      const double y = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<double>();
      const double z = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<double>();
      line.points.push_back({.x = x, .y = y, .z = z});
    }

    PJ::sdk::SceneEntity entity;
    entity.timestamp_ns = current_timestamp_;
    entity.frame_id = std::move(header.frame_id);
    entity.id = "polygon";
    entity.frame_locked = true;
    if (line.points.size() >= 2) {
      entity.lines.push_back(std::move(line));
    }
    PJ::sdk::SceneEntities entities;
    entities.entities.push_back(std::move(entity));
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(entities)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PolygonStamped: CDR read error: ") + e.what());
  }
}

namespace {

// nav2_costmap_2d's Costmap2DPublisher table: free 0, inscribed 253 -> 99, lethal 254 -> 100,
// no information 255 -> -1, and 1..252 spread over 1..98.
uint8_t nav2CostToOccupancy(uint8_t cost) {
  switch (cost) {
    case 0:
      return 0;
    case 253:
      return 99;
    case 254:
      return 100;
    case 255:
      return static_cast<uint8_t>(int8_t{-1});
    default:
      return static_cast<uint8_t>(1 + (97 * (cost - 1)) / 251);
  }
}

std::shared_ptr<std::vector<uint8_t>> nav2CostsToOccupancy(PJ::Span<const uint8_t> costs) {
  auto cells = std::make_shared<std::vector<uint8_t>>(costs.size());
  std::transform(costs.begin(), costs.end(), cells->begin(), nav2CostToOccupancy);
  return cells;
}

}  // namespace

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseNav2Costmap(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    auto header = readHeader();

    // CostmapMetaData: map_load_time and update_time (read and discarded; the grid uses the
    // Header stamp), layer name, resolution, size, origin.
    for (int i = 0; i < 2; ++i) {
      (void)deserializer_->deserialize(RosMsgParser::INT32);
      (void)deserializer_->deserialize(RosMsgParser::UINT32);
    }
    std::string layer;
    deserializer_->deserializeString(layer);
    const float resolution = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();
    const uint32_t size_x = deserializer_->deserializeUInt32();
    const uint32_t size_y = deserializer_->deserializeUInt32();
    const PJ::sdk::Pose origin = readPose();

    const auto costs = readByteSequence();
    if (static_cast<uint64_t>(size_x) * size_y != costs.size()) {
      return PJ::unexpected(std::string("Costmap: size_x * size_y does not match data length"));
    }

    auto cells = nav2CostsToOccupancy(PJ::Span<const uint8_t>(costs.data(), costs.size()));

    PJ::sdk::OccupancyGrid grid;
    grid.timestamp_ns = current_timestamp_;
    grid.frame_id = std::move(header.frame_id);
    grid.origin = origin;
    grid.resolution = static_cast<double>(resolution);
    grid.width = size_x;
    grid.height = size_y;
    grid.data = PJ::Span<const uint8_t>(cells->data(), cells->size());
    grid.anchor = std::move(cells);
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(grid)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Costmap: CDR read error: ") + e.what());
  }
}

// The window of a nav2 costmap that changed, placed like a map_msgs/OccupancyGridUpdate: x and y
// are the cell offset of its first cell into the costmap of the "<topic>" this "<topic>_updates"
// belongs to. Costs go through the same table as the full costmap.
PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseNav2CostmapUpdate(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    auto header = readHeader();

    const uint32_t x = deserializer_->deserializeUInt32();
    const uint32_t y = deserializer_->deserializeUInt32();
    const uint32_t size_x = deserializer_->deserializeUInt32();
    const uint32_t size_y = deserializer_->deserializeUInt32();
    const auto costs = readByteSequence();
    if (static_cast<uint64_t>(size_x) * size_y != costs.size()) {
      return PJ::unexpected(std::string("CostmapUpdate: size_x * size_y does not match data length"));
    }
    if (x > static_cast<uint32_t>(INT32_MAX) || y > static_cast<uint32_t>(INT32_MAX)) {
      return PJ::unexpected(std::string("CostmapUpdate: offset out of range"));
    }

    auto cells = nav2CostsToOccupancy(PJ::Span<const uint8_t>(costs.data(), costs.size()));

    PJ::sdk::OccupancyGridUpdate update;
    update.timestamp_ns = current_timestamp_;
    update.frame_id = std::move(header.frame_id);
    update.x = static_cast<int32_t>(x);
    update.y = static_cast<int32_t>(y);
    update.width = size_x;
    update.height = size_y;
    update.data = PJ::Span<const uint8_t>(cells->data(), cells->size());
    update.anchor = std::move(cells);
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(update)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CostmapUpdate: CDR read error: ") + e.what());
  }
}

// Byte-backed: the cell bytes are a zero-copy span over the payload, pinned by payload.anchor.
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

    const auto data_span = readByteSequence();

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

// x, y: offset of the patch's top-left cell into the base grid. The patch carries no origin/resolution;
// a stateful consumer places it at the base grid's origin + (x, y) * resolution (see
// occupancy_grid_update.hpp). The int8[] cells are byte-identical to uint8 on the wire, so the
// zero-copy span maps straight onto sdk::OccupancyGridUpdate::data.
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
    const auto data_span = readByteSequence();

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
    update.data = PJ::Span<const uint8_t>(data_span.data(), expected);
    update.anchor = payload.anchor;
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(update)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("OccupancyGridUpdate: CDR read error: ") + e.what());
  }
}

// grid_map_msgs/GridMap
//
// Wire layout (ROS 2 shown; ROS 1 prepends a uint32 seq inside the header):
//   info                  grid_map_msgs/GridMapInfo
//     header              std_msgs/Header      (handled by readHeader())
//     resolution          float64
//     length_x, length_y  float64
//     pose                geometry_msgs/Pose   (center; z + orientation ignored by grid_map_ros)
//   layers                string[]
//   basic_layers          string[]
//   data                  std_msgs/Float32MultiArray[]   (one per layer)
//     layout.dim          MultiArrayDimension[] {string label; uint32 size; uint32 stride}
//     layout.data_offset  uint32
//     data                float32[]
//   outer_start_index     uint16
//   inner_start_index     uint16
//
// Each layer's cell window is copied into per-instance scratch for the pj_grid_map transcoder,
// which owns the axis flip / ring-buffer math and the label/geometry checks (see transcodeGridMap).
// Counts are validated against the caps and the bytes left BEFORE any allocation, so a corrupt
// length prefix cannot request gigabytes; a truncated wire throws out of the deserializer and
// becomes an Expected error.
RosParser::HeaderData RosParser::readGridMapMessage(
    PJ::grid_map::GridMapMessage& out, PJ::sdk::Pose& center, bool skip_layer_data, bool little_endian_wire) {
  using PJ::grid_map::kMaxCells;
  using PJ::grid_map::kMaxLayers;
  HeaderData header = readHeader();
  out.resolution = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  out.length_x = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  out.length_y = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  center = readPose();
  out.center_x = center.position.x;  // z + orientation: grid_map_ros ignores them
  out.center_y = center.position.y;

  const auto read_strings = [this](std::vector<std::string>& names, const char* what) {
    const uint32_t count = deserializer_->deserializeUInt32();
    if (count > kMaxLayers) {
      throw std::runtime_error(std::string("GridMap: too many ") + what);
    }
    names.resize(count);
    for (auto& name : names) {
      deserializer_->deserializeString(name);
    }
  };
  read_strings(out.layers, "layers");
  read_strings(out.basic_layers, "basic_layers");

  const uint32_t array_count = deserializer_->deserializeUInt32();
  if (array_count != out.layers.size()) {
    throw std::runtime_error("GridMap: layer count differs from data array count");
  }
  if (!skip_layer_data) {
    gridmap_layer_scratch_.resize(array_count);
  }
  out.data.resize(array_count);
  for (uint32_t k = 0; k < array_count; ++k) {
    auto& layer = out.data[k];
    if (deserializer_->deserializeUInt32() != 2) {
      throw std::runtime_error("GridMap: layer layout must have exactly two dims");
    }
    layer.dims.resize(2);
    for (auto& dim : layer.dims) {
      deserializer_->deserializeString(dim.label);
      dim.size = deserializer_->deserializeUInt32();
      dim.stride = deserializer_->deserializeUInt32();
    }
    const uint64_t cells = static_cast<uint64_t>(layer.dims[0].size) * layer.dims[1].size;
    if (cells == 0 || cells > kMaxCells) {
      throw std::runtime_error("GridMap: cell count out of range");
    }
    const uint32_t data_offset = deserializer_->deserializeUInt32();
    const uint32_t float_count = deserializer_->deserializeUInt32();
    requireAvailable(*deserializer_, float_count, sizeof(float), "layer data");
    if (skip_layer_data) {
      deserializer_->jump(static_cast<size_t>(float_count) * sizeof(float));
      continue;
    }
    if (data_offset + cells > float_count) {
      throw std::runtime_error("GridMap: layer array is shorter than its layout");
    }
    // Only the cell window is copied (leading padding and trailing elements are skipped); the
    // scratch keeps the message borrowable after the payload.
    auto& floats = gridmap_layer_scratch_[k];
    floats.resize(static_cast<size_t>(cells));
    if (little_endian_wire) {
      // The floats are contiguous right after their count (which leaves the
      // CDR cursor 4-aligned), so the window is one bulk copy.
      deserializer_->jump(static_cast<size_t>(data_offset) * sizeof(float));
      std::memcpy(floats.data(), deserializer_->getCurrentPtr(), floats.size() * sizeof(float));
      deserializer_->jump((static_cast<size_t>(float_count) - data_offset) * sizeof(float));
    } else {
      for (uint64_t i = 0; i < float_count; ++i) {
        const float value = deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>();
        if (i >= data_offset && i < data_offset + cells) {
          floats[static_cast<size_t>(i - data_offset)] = value;
        }
      }
    }
    layer.data_offset = 0;
    layer.data = PJ::Span<const float>(floats.data(), floats.size());
  }
  out.outer_start_index = deserializer_->deserialize(RosMsgParser::UINT16).extract<uint16_t>();
  out.inner_start_index = deserializer_->deserialize(RosMsgParser::UINT16).extract<uint16_t>();
  return header;
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseGridMap(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    // NanoCDR byte-swaps scalars when the encapsulation header (payload byte 1,
    // low bit) says big-endian; the bulk float copy cannot, so it is reserved
    // for ROS 1 and little-endian CDR.
    const bool little_endian_wire = use_ros1_ || (payload.bytes.size() > 1 && (payload.bytes[1] & 1u) != 0);
    PJ::grid_map::GridMapMessage msg;
    PJ::sdk::Pose center;
    HeaderData header = readGridMapMessage(msg, center, /*skip_layer_data=*/false, little_endian_wire);
    auto grid = PJ::grid_map::transcodeGridMap(msg);
    if (!grid) {
      return PJ::unexpected(std::move(grid).error());
    }
    grid->frame_id = std::move(header.frame_id);
    grid->timestamp_ns = current_timestamp_;
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(*grid)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("GridMap: CDR read error: ") + e.what());
  }
}

PJ::Status RosParser::parseGridMapScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);
    PJ::grid_map::GridMapMessage msg;
    PJ::sdk::Pose center;
    emitHeader(readGridMapMessage(msg, center, /*skip_layer_data=*/true, /*little_endian_wire=*/false));
    addField("/info/resolution", msg.resolution);
    addField("/info/length_x", msg.length_x);
    addField("/info/length_y", msg.length_y);
    path_.clear();
    emitPose("/info/pose", center);
    // Only the layer count is plottable, not the names; size_x / size_y live in
    // the first layer's layout (dim[1] / dim[0]).
    addField("/num_layers", static_cast<double>(msg.layers.size()));
    if (!msg.data.empty() && msg.data.front().dims.size() == 2) {
      addField("/size_x", static_cast<double>(msg.data.front().dims[1].size));
      addField("/size_y", static_cast<double>(msg.data.front().dims[0].size));
    }
    return PJ::okStatus();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("GridMap scalars: CDR read error: ") + e.what());
  }
}

// The pose is the corner of cell (0,0) and is kept as the origin. The packed layout is the canonical
// one, so data[] is a view over the payload pinned by its anchor. The wire has no row count;
// finalizeFoxgloveGrid derives it and validates the layout.
PJ::Expected<PJ::sdk::GridMap> RosParser::readFoxgloveGrid(PJ::sdk::BufferAnchor anchor) {
  PJ::sdk::GridMap grid;
  grid.timestamp_ns = readBareTime();
  deserializer_->deserializeString(grid.frame_id);
  grid.origin = readPose();
  grid.column_count = deserializer_->deserializeUInt32();
  grid.cell_size.x = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  grid.cell_size.y = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  grid.row_stride = deserializer_->deserializeUInt32();
  grid.cell_stride = deserializer_->deserializeUInt32();

  const uint32_t field_count = deserializer_->deserializeUInt32();
  if (field_count > 1024) {
    return PJ::unexpected(std::string("Grid: too many fields"));
  }
  requireAvailable(*deserializer_, field_count, 2 * sizeof(uint32_t) + 1, "fields");
  grid.fields.resize(field_count);
  for (auto& field : grid.fields) {
    deserializer_->deserializeString(field.name);
    field.offset = deserializer_->deserializeUInt32();
    field.datatype = PJ::grid_map::foxgloveNumericTypeToPointField(readU8(*deserializer_));
    field.count = 1;
  }
  const auto data_span = readByteSequence();
  grid.data = PJ::Span<const uint8_t>(data_span.data(), data_span.size());
  grid.anchor = std::move(anchor);
  if (auto ok = PJ::grid_map::finalizeFoxgloveGrid(grid); !ok) {
    return PJ::unexpected(std::string("Grid: ") + std::move(ok).error());
  }
  return grid;
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseFoxgloveGrid(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    auto grid = readFoxgloveGrid(payload.anchor);
    if (!grid) {
      return PJ::unexpected(std::move(grid).error());
    }
    // The object carries the envelope stamp like every other handler; the
    // embedded one only reaches current_timestamp_ through readBareTime()'s
    // adoption (and the scalar route's /timestamp column).
    grid->timestamp_ns = current_timestamp_;
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(*grid)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Grid: CDR read error: ") + e.what());
  }
}

PJ::Status RosParser::parseFoxgloveGridScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);
    // The full decode is a few scalar reads plus a length prefix (data[] is aliased), so the
    // scalar route reuses it.
    auto grid = readFoxgloveGrid(nullptr);
    if (!grid) {
      return PJ::unexpected(std::move(grid).error());
    }
    addField("/timestamp", nanosecondsToSeconds(grid->timestamp_ns));
    addStringField("/frame_id", grid->frame_id);
    addField("/column_count", PJ::sdk::ValueRef{static_cast<uint64_t>(grid->column_count)});
    addField("/row_count", PJ::sdk::ValueRef{static_cast<uint64_t>(grid->row_count)});
    addField("/row_stride", PJ::sdk::ValueRef{static_cast<uint64_t>(grid->row_stride)});
    addField("/cell_stride", PJ::sdk::ValueRef{static_cast<uint64_t>(grid->cell_stride)});
    addField("/data_size", PJ::sdk::ValueRef{static_cast<uint64_t>(grid->data.size())});
    return PJ::okStatus();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Grid scalars: CDR read error: ") + e.what());
  }
}

// std_msgs/String on a robot_description topic (dispatched in bindSchema by topic name; a generic
// String stays generic). The body is the URDF/SDF/MJCF source, carried verbatim plus a best-effort
// format hint sniffed from the root element; consumers do the format-specific parsing.
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
