/**
 * @file ros_yolo_handlers.cpp
 * @brief yolo_msgs/DetectionArray -> sdk::ImageAnnotations (per-message,
 *        stateless decode). See parser_ros/YOLO_NOTES.md for the design
 *        rationale.
 *
 * Each Detection becomes a bounding-box rectangle (PointsAnnotation, kLineLoop)
 * plus a class/score label (TextAnnotation); when present, the segmentation-mask
 * boundary is drawn as another kLineLoop and the 2D pose keypoints as small
 * filled circles. The decode is positional CDR — every field of every Detection
 * is consumed in full so the next Detection in the array stays aligned, even the
 * fields we don't render (bbox3d, keypoints3d, theta, mask dimensions).
 *
 * yolo_msgs (github.com/mgonzs13/yolo_ros) is a third-party message, NOT part of
 * standard ROS. This handler is net-new functionality, not a port of an upstream
 * PlotJuggler plugin — see YOLO_NOTES.md.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <pj_base/builtin/image_annotations.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

namespace {

// Reject pathological array sizes from corrupt/hostile payloads before allocating.
constexpr uint32_t kMaxDetections = 100'000;
constexpr uint32_t kMaxMaskPoints = 2'000'000;
constexpr uint32_t kMaxKeypoints = 100'000;

// Pixel radius for a rendered keypoint dot.
constexpr double kKeypointRadius = 3.0;

// Distinct, high-contrast colors cycled by class id (a=255, opaque).
constexpr PJ::sdk::ColorRGBA kClassPalette[] = {
    {0xE6, 0x19, 0x4B, 255}, {0x3C, 0xB4, 0x4B, 255}, {0xFF, 0xE1, 0x19, 255}, {0x43, 0x63, 0xD8, 255},
    {0xF5, 0x82, 0x31, 255}, {0x91, 0x1E, 0xB4, 255}, {0x46, 0xF0, 0xF0, 255}, {0xF0, 0x32, 0xE6, 255},
    {0xBF, 0xEF, 0x45, 255}, {0xFA, 0xBE, 0xD4, 255}, {0x46, 0x99, 0x90, 255}, {0xDC, 0xBE, 0xFF, 255},
};
constexpr int32_t kClassPaletteSize = 12;

PJ::sdk::ColorRGBA colorForClass(int32_t class_id) {
  const int32_t idx = ((class_id % kClassPaletteSize) + kClassPaletteSize) % kClassPaletteSize;
  return kClassPalette[static_cast<size_t>(idx)];
}

// Clamp an array reservation by the bytes actually remaining. An element cannot
// occupy fewer than `min_wire_bytes`, so however large (but in-cap) a corrupt
// count is, we never reserve more than the payload could possibly hold — this
// defuses the allocation amplification of reserving on an untrusted count.
uint32_t reserveHint(uint32_t count, size_t bytes_left, uint32_t min_wire_bytes) {
  return std::min(count, static_cast<uint32_t>(bytes_left / min_wire_bytes));
}

std::string formatLabel(const std::string& class_name, double score) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f", score);
  return class_name.empty() ? std::string(buf) : class_name + " " + buf;
}

}  // namespace

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseYoloDetectionArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));

    auto rf64 = [&] { return deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>(); };
    auto ri32 = [&] { return deserializer_->deserialize(RosMsgParser::INT32).convert<int32_t>(); };

    const HeaderData header = readHeader();

    PJ::sdk::ImageAnnotations out;
    out.timestamp = current_timestamp_;
    // Informational only — the Scene2D renderer composites by layer stacking,
    // not by matching this against an image topic. yolo_msgs carries no image
    // topic name, so the camera optical frame_id is the best available hint.
    out.image_topic = header.frame_id;

    const uint32_t num_detections = deserializer_->deserializeUInt32();
    if (num_detections > kMaxDetections) {
      throw std::runtime_error("DetectionArray detections[] exceeds sanity cap");
    }
    // A Detection occupies far more than 64 bytes on the wire (the fixed
    // bbox2d/bbox3d scalar block alone is ~120), so 64 is a safe lower bound.
    const uint32_t detection_hint = reserveHint(num_detections, deserializer_->bytesLeft(), 64);
    out.points.reserve(detection_hint);
    out.texts.reserve(detection_hint);

    for (uint32_t d = 0; d < num_detections; ++d) {
      const int32_t class_id = ri32();
      std::string class_name;
      deserializer_->deserializeString(class_name);
      const double score = rf64();
      std::string track_id;
      deserializer_->deserializeString(track_id);

      // BoundingBox2D = Pose2D{Point2D{x,y}, theta} + Vector2{x,y}, in pixels.
      const double cx = rf64();
      const double cy = rf64();
      (void)rf64();  // theta — not rendered
      const double sx = rf64();
      const double sy = rf64();

      // BoundingBox3D = Pose{Point(3) + Quaternion(4)} + Vector3(3) + frame_id.
      // Consumed in full to stay aligned; not rendered (2D overlay).
      for (int k = 0; k < 10; ++k) {
        (void)rf64();
      }
      std::string bbox3d_frame;
      deserializer_->deserializeString(bbox3d_frame);

      // Mask = int32 height + int32 width + Point2D[] data (boundary points).
      (void)ri32();  // height
      (void)ri32();  // width
      const uint32_t num_mask = deserializer_->deserializeUInt32();
      if (num_mask > kMaxMaskPoints) {
        throw std::runtime_error("Detection mask data[] exceeds sanity cap");
      }
      std::vector<PJ::sdk::Point2> mask_points;
      mask_points.reserve(reserveHint(num_mask, deserializer_->bytesLeft(), 16));  // Point2D = 2 × float64
      for (uint32_t i = 0; i < num_mask; ++i) {
        const double mx = rf64();
        const double my = rf64();
        mask_points.push_back({.x = mx, .y = my});
      }

      // KeyPoint2DArray = KeyPoint2D[] {int32 id, Point2D{x,y}, float64 score}.
      const uint32_t num_kp2 = deserializer_->deserializeUInt32();
      if (num_kp2 > kMaxKeypoints) {
        throw std::runtime_error("Detection keypoints[] exceeds sanity cap");
      }
      std::vector<PJ::sdk::Point2> keypoints;
      keypoints.reserve(reserveHint(num_kp2, deserializer_->bytesLeft(), 28));  // KeyPoint2D = 4 + 16 + 8 bytes
      for (uint32_t i = 0; i < num_kp2; ++i) {
        (void)ri32();  // id
        const double kx = rf64();
        const double ky = rf64();
        (void)rf64();  // score
        keypoints.push_back({.x = kx, .y = ky});
      }

      // KeyPoint3DArray = KeyPoint3D[] {int32 id, Point{x,y,z}, float64 score} + frame_id.
      // Consumed in full; not rendered (2D overlay).
      const uint32_t num_kp3 = deserializer_->deserializeUInt32();
      if (num_kp3 > kMaxKeypoints) {
        throw std::runtime_error("Detection keypoints3d[] exceeds sanity cap");
      }
      for (uint32_t i = 0; i < num_kp3; ++i) {
        (void)ri32();  // id
        (void)rf64();  // x
        (void)rf64();  // y
        (void)rf64();  // z
        (void)rf64();  // score
      }
      std::string kp3_frame;
      deserializer_->deserializeString(kp3_frame);

      // --- Build the rendered annotations from the fields we keep. ---
      const PJ::sdk::ColorRGBA color = colorForClass(class_id);
      const double hx = sx / 2.0;
      const double hy = sy / 2.0;

      PJ::sdk::PointsAnnotation box;
      box.topology = PJ::sdk::AnnotationTopology::kLineLoop;
      box.points = {
          {.x = cx - hx, .y = cy - hy},
          {.x = cx + hx, .y = cy - hy},
          {.x = cx + hx, .y = cy + hy},
          {.x = cx - hx, .y = cy + hy},
      };
      box.color = color;
      out.points.push_back(std::move(box));

      PJ::sdk::TextAnnotation label;
      label.position = {.x = cx - hx, .y = cy - hy};
      label.text = formatLabel(class_name, score);
      label.color = color;
      out.texts.push_back(std::move(label));

      if (!mask_points.empty()) {
        PJ::sdk::PointsAnnotation mask;
        mask.topology = PJ::sdk::AnnotationTopology::kLineLoop;
        mask.points = std::move(mask_points);
        mask.color = color;
        out.points.push_back(std::move(mask));
      }

      for (const PJ::sdk::Point2& kp : keypoints) {
        PJ::sdk::CircleAnnotation circle;
        circle.center = kp;
        circle.radius = kKeypointRadius;
        circle.color = color;
        circle.fill_color = color;
        out.circles.push_back(std::move(circle));
      }
    }

    // A corrupt-but-in-bounds count can desync the positional CDR decode without
    // overrunning, so the try/catch never sees it; trailing bytes are the only
    // signature. Warn once and still return best-effort — a hard throw would risk
    // rejecting valid frames, since bytesLeft() > 0 is ambiguous (desync vs CDR
    // alignment slack) and no other handler asserts an exact end of buffer.
    if (deserializer_->bytesLeft() != 0 && !yolo_trailing_warned_) {
      yolo_trailing_warned_ = true;
      std::fprintf(
          stderr,
          "[ros_parser] yolo_msgs/DetectionArray: %zu trailing byte(s) after decode; "
          "possible yolo_msgs layout/version mismatch (rendered best-effort).\n",
          deserializer_->bytesLeft());
    }

    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(out)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("DetectionArray: CDR read error: ") + e.what());
  }
}

// Slim scalar companion: read just the header + the detections[] count and emit
// `num_detections`. Keeps the host's eager-scalar ingest from aborting on this
// object-only schema (see parseScalarsObjectOnly) while giving a useful plottable
// count. The full per-detection decode stays in parseYoloDetectionArray.
PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parseYoloScalars(
    PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()));
    (void)readHeader();
    const uint32_t num_detections = deserializer_->deserializeUInt32();
    std::vector<PJ::sdk::NamedFieldValue> out;
    out.push_back({.name = "num_detections", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(num_detections)}});
    return out;
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("DetectionArray scalars: CDR read error: ") + e.what());
  }
}

}  // namespace ros_parser_detail
