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
#include <pj_grid_map/grid_map_transcoder.hpp>
#include <pj_pointcloud_color/pointcloud_color.hpp>
#include <stdexcept>
#include <unordered_map>

#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

namespace {

// Bytes per pixel for the raw ROS image encodings parser_ros consumes. Used
// only to validate that row_step >= width * bpp. Encoding strings are
// emitted into Image::encoding verbatim — the consumer routes by string.
// Bayer CFA encodings (bayer_*8) carry one raw mosaic sample per pixel, so they
// are 1 byte/pixel here; demosaicing to RGB happens downstream in the viewer.
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

// ---------------------------------------------------------------------------
// sensor_msgs/Image
//
// Wire layout (ROS2 shown; ROS1 prepends a uint32 seq inside the header):
//   header                  std_msgs/Header  (handled by readHeader())
//   height                  uint32
//   width                   uint32
//   encoding                string (e.g. "rgb8", "bgr8", "mono8", "mono16", "16UC1", "bayer_rggb8", …)
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
    const auto data_span = readByteSequence();
    const uint8_t* src = data_span.data();
    const uint32_t data_len = static_cast<uint32_t>(data_span.size());

    std::string out_encoding;
    std::optional<float> depth_min;
    std::optional<float> depth_max;
    size_t blob_offset = 0;
    uint32_t blob_size = data_len;

    if (format.find("compressedDepth") != std::string::npos) {
      out_encoding = "compressedDepth";  // PNG/RVL payload (+ optional quantization range).
      // compressedDepth normally prefixes the PNG/RVL blob with a 12-byte
      // ConfigHeader (uint32 format, float depthQuantA, float depthQuantB). But
      // some recorders emit a BARE PNG with no ConfigHeader (seen in RealSense
      // bags); stripping 12 bytes there chops the PNG's 8-byte signature + part
      // of the IHDR length and corrupts it. A leading PNG signature therefore
      // means "headerless" — pass the blob through untouched.
      static constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
      const bool headerless_png =
          data_len >= sizeof(kPngSignature) && std::memcmp(src, kPngSignature, sizeof(kPngSignature)) == 0;
      if (headerless_png) {
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
    // readBareTime() reads `sec` SIGNED (int32) and applies the embedded-stamp
    // adoption; reading it unsigned turned negative stamps into ~4.29e9.
    (void)readBareTime();

    std::string frame_id;
    deserializer_->deserializeString(frame_id);
    const auto data_span = readByteSequence();
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
    const auto data_span = readByteSequence();

    // is_dense follows data[]. deserializeByteSequence advanced the cursor
    // past the body, so the next byte read is is_dense itself.
    bool is_dense = true;
    if (deserializer_->bytesLeft() >= 1) {
      is_dense = (readU8(*deserializer_) != 0);
    }

    // Zero-copy by default: data_span is a slice of the payload. For a PointCloud2 with
    // a few MB of points this is the win — no per-message alloc/copy on the hot path.
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
    // Normalize colour to the canonical packed "rgba" field so the host renders one
    // per-point colour instead of offering each channel as a separate colormap source.
    // A PCL packed rgb/rgba (0x00RRGGBB) is repacked into canonical R,G,B,A order in a
    // fresh owned buffer (zero-copy given up only for colour clouds); separate
    // red/green/blue/alpha channels collapse zero-copy; plain XYZI clouds are untouched.
    pj::pointcloud_color::normalizeCanonicalColor(cloud);

    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(cloud)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PointCloud2: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Slim scalar route for sensor_msgs/PointCloud2.
//
// Emits the header series (/header/stamp, /header/frame_id, plus /header/seq on
// ROS 1) and /num_points — everything else on this wire is layout detail of a
// payload consumed as a 3D object, not as time series.
//
// /num_points is the number of points ACTUALLY present in the message:
// len(data) / point_step. It is deliberately NOT width * height. Those two agree
// only for a well-formed dense cloud; a truncated or partially-filled message
// (and any producer that leaves width/height as the sensor's nominal grid) makes
// width * height a claim about the cloud rather than a measurement of it — the
// series is worth plotting precisely because it can reveal dropped points.
// Reaching data[] means walking past fields[] to point_step, which is a handful
// of cheap reads; the bulk bytes themselves are never touched (we read only the
// sequence's length prefix and stop).
//
// This replaces the generic DISCARD_LARGE_ARRAYS flatten: that walked the whole
// message through rosx_introspection on every frame just to yield per-PointField
// metadata columns, and — being a flatten of the leaf fields — never produced
// the combined /header/stamp series at all.
// ---------------------------------------------------------------------------

PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parsePointCloud2Scalars(
    PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);
    emitHeader(readHeader());

    (void)deserializer_->deserializeUInt32();  // height
    (void)deserializer_->deserializeUInt32();  // width

    // fields[] — read and discard; we only need the cursor to land on point_step.
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

    // data[] length prefix only — the points themselves are never read here.
    const uint32_t data_size = deserializer_->deserializeUInt32();
    if (data_size > deserializer_->bytesLeft()) {
      // Declared length runs past the payload: truncated or corrupt. Same
      // verdict readByteSequence() reaches on the object route.
      return PJ::unexpected(std::string("PointCloud2 scalars: data[] length exceeds message payload"));
    }
    // point_step == 0 is malformed (no point has zero size). Omit the column for
    // that message rather than dividing by zero or filing a fabricated count.
    if (point_step > 0) {
      addField("/num_points", PJ::sdk::ValueRef{static_cast<uint64_t>(data_size) / point_step});
    }
    return harvestOwnedFields();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PointCloud2 scalars: CDR read error: ") + e.what());
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
    // readBareTime() reads `sec` SIGNED (int32) and applies the embedded-stamp
    // adoption; reading it unsigned turned negative stamps into ~4.29e9.
    (void)readBareTime();

    std::string frame_id;
    deserializer_->deserializeString(frame_id);

    // geometry_msgs/Pose — 7 doubles (position xyz, orientation xyzw). Read to
    // advance the cursor, then drop: the canonical object carries no pose.
    for (int i = 0; i < 7; ++i) {
      (void)deserializer_->deserialize(RosMsgParser::FLOAT64);
    }

    const auto data_span = readByteSequence();
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

// Slim scalar companion for foxglove_msgs/CompressedPointCloud. The head of the
// wire is a BARE builtin_interfaces/Time followed by frame_id — NOT a
// std_msgs/Header — so readHeader() must not be used here: its ROS 1 branch
// would consume `seq` first and shift every subsequent read. We therefore emit
// the flat "/timestamp" + "/frame_id" pair rather than the /header/* series.
// There is no point count anywhere in this schema (the cloud is an opaque
// compressed blob), so nothing else is emitted.
PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parseFoxgloveCompressedPointCloudScalars(
    PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);

    // readBareTime() reads `sec` SIGNED (int32) and applies the embedded-stamp
    // adoption under the same contract as readHeader(): the stamp becomes the
    // record time only when the user asked for it (registerBoundSchemaHandler
    // reads current_timestamp_ back out).
    const int64_t stamp_ns = readBareTime();
    addField("/timestamp", nanosecondsToSeconds(stamp_ns));

    std::string frame_id;
    deserializer_->deserializeString(frame_id);
    addStringField("/frame_id", frame_id);

    return harvestOwnedFields();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CompressedPointCloud scalars: CDR read error: ") + e.what());
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
    const auto data_span = readByteSequence();
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
// Emitted as a canonical sdk::FrameTransforms (owned — no byte blob). The 3D
// scene's TF buffer keys each edge by FrameTransform::timestamp and scrubs it
// with a zero-order hold at the playhead, which lives on the MCAP message clock
// (log / publish time). readStampedTransform() therefore resolves the per-sample
// timestamp so it lands on that same clock — see the three cases there. The
// scalar handler (handleTFMessage) still runs in parallel for users who want to
// plot the transforms as time series, and it always keeps the raw Header.stamp.
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

  const int64_t header_stamp_ns = static_cast<int64_t>(header.sec) * 1000000000LL + static_cast<int64_t>(header.nsec);

  PJ::sdk::FrameTransform tf;
  // Timestamp resolution, so the TF buffer lands on the same clock the 3D scene
  // scrubs on (see this section's header comment):
  //   1. "use embedded timestamp" ON  -> the payload Header.stamp, honoring the
  //      toggle exactly as every other message type does.
  //   2. OFF + Header.stamp == 0       -> keep 0: a static transform (/tf_static
  //      latched with an unset stamp) whose single sample must hold for every
  //      query time via the buffer's zero-order hold.
  //   3. OFF + Header.stamp != 0       -> current_timestamp_, the MCAP-resolved
  //      message time (log / publish). A bag recorded across unsynchronized
  //      clocks carries a Header.stamp on a different clock — in offset AND rate —
  //      than the message time the playhead uses; keying the TF buffer by that
  //      stamp drifts transforms out of sync with point clouds. Overriding onto
  //      the message clock keeps them moving together.
  if (use_embedded_timestamp_ || header_stamp_ns == 0) {
    tf.timestamp = static_cast<PJ::Timestamp>(header_stamp_ns);
  } else {
    tf.timestamp = current_timestamp_;
  }
  tf.parent_frame_id = std::move(header.frame_id);
  tf.child_frame_id = std::move(child_frame_id);
  tf.translation = {.x = tx, .y = ty, .z = tz};
  tf.rotation = {.x = qx, .y = qy, .z = qz, .w = qw};
  return tf;
}

// geometry_msgs/Pose on the wire: position (Point: x/y/z f64) then orientation
// (Quaternion: x/y/z/w f64) — 7 doubles, no header.
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
// geometry_msgs/PoseArray
//
// Wire layout:
//   header        std_msgs/Header       (sec, nanosec, frame_id — readHeader())
//   poses         geometry_msgs/Pose[]  (uint32 count + N poses)
//
// Emitted as a canonical sdk::PosesInFrame (owned — no byte blob). The
// canonical type stores ONLY poses: rendering style (arrow vs triad, size,
// color) is chosen by the viewer at draw time.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// foxglove_msgs/PosesInFrame
//
// Wire layout (ROS2 CDR):
//   timestamp     builtin_interfaces/Time  (sec int32, nanosec uint32)
//   frame_id      string
//   poses         geometry_msgs/Pose[]     (uint32 count + N poses)
//
// Like foxglove_msgs/CompressedVideo, the first field is a BARE
// builtin_interfaces/Time, not a std_msgs/Header — so readHeader() must NOT be
// used (it would consume the wrong fields). We read the two Time words directly.
// ---------------------------------------------------------------------------

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseFoxglovePosesInFrame(
    PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    // builtin_interfaces/Time timestamp — bare, not wrapped in a Header.
    // readBareTime() reads `sec` SIGNED (int32) and applies the embedded-stamp
    // adoption; reading it unsigned turned negative stamps into ~4.29e9.
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

// ---------------------------------------------------------------------------
// geometry_msgs/PoseStamped — a single stamped pose, surfaced as a one-element
// PosesInFrame so it feeds the same 3D pose view as PoseArray. The scalar
// handler (handlePoseStamped) still runs in parallel for per-axis plotting.
//
// Wire layout:
//   header   std_msgs/Header       (sec, nanosec, frame_id — readHeader())
//   pose     geometry_msgs/Pose    (position xyz + orientation xyzw)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// nav_msgs/Odometry — the pose of child_frame_id expressed in the header frame,
// surfaced as a one-element PosesInFrame so it feeds the same 3D pose view as
// PoseStamped. The scalar handler (handleOdometry) still runs in parallel for
// per-axis plotting of the pose, twist and covariances.
//
// Wire layout:
//   header         std_msgs/Header                   (sec, nanosec, frame_id)
//   child_frame_id string
//   pose           geometry_msgs/PoseWithCovariance  (Pose then float64[36])
//   twist          geometry_msgs/TwistWithCovariance
//
// Only the Header + child_frame_id + Pose are consumed: the pose is the last
// field the object needs, so the covariance and twist are left unread. The
// object adopts the HEADER frame_id (the reference frame), not child_frame_id.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// nav_msgs/Path
//
// Wire layout:
//   header   std_msgs/Header               (path frame_id + stamp)
//   poses    geometry_msgs/PoseStamped[]    (uint32 count + N)
//     each PoseStamped: std_msgs/Header header; geometry_msgs/Pose pose
//
// A PosesInFrame is an array of poses in ONE frame at ONE instant, so the
// object adopts the PATH header's frame_id + stamp. Each PoseStamped's own
// Header is read and dropped (readHeader advances the cursor; its
// current_timestamp_ side-effect is overwritten by the path stamp captured up
// front). Per-pose stamps/frames remain available via the generic scalar route.
// ---------------------------------------------------------------------------

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
// Each layer's cell window is copied into per-instance scratch and handed to
// the pj_grid_map transcoder, which owns the axis flip / ring-buffer math and
// the label/geometry checks (see transcodeGridMap). Counts are validated
// against the caps and the bytes left BEFORE any allocation so a corrupt
// length prefix cannot request gigabytes; a genuinely truncated wire throws
// out of the deserializer and becomes an Expected error.
// ---------------------------------------------------------------------------

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
    // Only the cell window is copied; leading padding and trailing elements
    // are skipped on the wire. The scratch keeps the message borrowable after
    // the payload.
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

PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parseGridMapScalars(
    PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);
    PJ::grid_map::GridMapMessage msg;
    PJ::sdk::Pose center;
    emitHeader(readGridMapMessage(msg, center, /*skip_layer_data=*/true, /*little_endian_wire=*/false));
    addField("/info/resolution", msg.resolution);
    addField("/info/length_x", msg.length_x);
    addField("/info/length_y", msg.length_y);
    emitPose("/info/pose", center);
    // Only the layer count is plottable, not the names; size_x / size_y live in
    // the first layer's layout (dim[1] / dim[0]).
    addField("/num_layers", static_cast<double>(msg.layers.size()));
    if (!msg.data.empty() && msg.data.front().dims.size() == 2) {
      addField("/size_x", static_cast<double>(msg.data.front().dims[1].size));
      addField("/size_y", static_cast<double>(msg.data.front().dims[0].size));
    }
    return harvestOwnedFields();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("GridMap scalars: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// foxglove_msgs/Grid
//
// Wire layout:
//   timestamp     bare time   (ROS 2: int32 sec, uint32 nanosec; ROS 1: uint32 sec, uint32 nsec)
//   frame_id      string
//   pose          geometry_msgs/Pose   (corner of cell (0,0) -> origin, kept as-is)
//   column_count  uint32
//   cell_size     foxglove_msgs/Vector2 (float64 x, y)
//   row_stride    uint32
//   cell_stride   uint32
//   fields        foxglove_msgs/PackedElementField[] {string name; uint32 offset; uint8 type}
//   data          uint8[]   <- row-major packed cells, zero-copied
//
// The packed layout is the canonical one, so data[] is a view over the payload
// pinned by its anchor. There is no row count on the wire; finalizeFoxgloveGrid
// derives it and validates the layout.
// ---------------------------------------------------------------------------

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

PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parseFoxgloveGridScalars(
    PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);
    // The full decode is a handful of scalar reads plus a length prefix (data[]
    // is aliased, never copied), so the scalar route reuses it.
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
    return harvestOwnedFields();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Grid scalars: CDR read error: ") + e.what());
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
