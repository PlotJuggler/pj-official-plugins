#pragma once
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Decoders for the well-known Foxglove protobuf schemas into their canonical
// PlotJuggler builtin objects, so a Foxglove .mcap renders the same way a ROS
// bag does. Same approach as foxglove_pointcloud_codec.hpp: the wire layout is
// known (extracted from the schemas in the file), so we decode it directly with
// google::protobuf's CodedInputStream instead of the reflection/descriptor pool.
//
// Mapping (foxglove schema -> sdk builtin object -> BuiltinObjectType):
//   foxglove.FrameTransform     -> sdk::FrameTransforms  (kFrameTransforms)
//   foxglove.CompressedImage    -> sdk::Image            (kImage)          [zero-copy data]
//   foxglove.CameraCalibration  -> sdk::CameraInfo       (kCameraInfo)
//   foxglove.ImageAnnotations   -> sdk::ImageAnnotations (kImageAnnotations)
//   foxglove.SceneUpdate        -> sdk::SceneEntities    (kSceneEntities)
//
// Two recurring conversions vs the SDK structs:
//   - Color: foxglove uses double r/g/b/a in [0,1]; sdk::ColorRGBA is uint8 0..255.
//   - Topology enums differ (see the .cpp remap tables).

#include <cstddef>
#include <cstdint>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/builtin/camera_info.hpp>
#include <pj_base/builtin/frame_transforms.hpp>
#include <pj_base/builtin/image.hpp>
#include <pj_base/builtin/image_annotations.hpp>
#include <pj_base/builtin/scene_entities.hpp>
#include <pj_base/expected.hpp>

namespace google::protobuf {
class Descriptor;
}  // namespace google::protobuf

namespace pj_protobuf {

// ---------------------------------------------------------------------------
// Descriptor-driven field numbers.
//
// The decoders below are hand-rolled zero-copy scanners that read the protobuf
// wire by field NUMBER. The well-known Foxglove schemas have official numbers,
// but a self-describing .mcap is free to embed a schema that renumbers them
// (observed in real DROID-dataset files written by mcap-rs, which move `frame_id`
// to the end and shift the others down). The dispatcher keys off the schema NAME
// only, so without honoring the embedded descriptor the scanner mis-reads every
// message of such a file.
//
// Each `*FieldNumbers` struct defaults to that codec's historical hardcoded
// numbering (so schemaless streams with no descriptor behave exactly as before),
// and `resolve*FieldNumbers(descriptor)` overrides any field whose NAME is found
// in the embedded message descriptor. A null descriptor yields all defaults.
// ---------------------------------------------------------------------------

/// Field numbers for foxglove.RawImage (defaults = official Foxglove numbering).
struct RawImageFieldNumbers {
  int timestamp = 1;
  int frame_id = 2;
  int width = 3;
  int height = 4;
  int encoding = 5;
  int step = 6;
  int data = 7;
};
[[nodiscard]] RawImageFieldNumbers resolveRawImageFieldNumbers(const google::protobuf::Descriptor* descriptor);

/// Field numbers for foxglove.CompressedImage (`format` is the foxglove name for
/// the sdk `encoding`). Defaults = official Foxglove numbering.
struct CompressedImageFieldNumbers {
  int timestamp = 1;
  int data = 2;
  int format = 3;
  int frame_id = 4;
};
[[nodiscard]] CompressedImageFieldNumbers resolveCompressedImageFieldNumbers(
    const google::protobuf::Descriptor* descriptor);

/// Field numbers for foxglove.CameraCalibration. Defaults mirror the codec's
/// historical hardcoded numbering (frame_id last); the descriptor overrides them,
/// so official-numbered files (frame_id=2) now decode too.
struct CameraCalibrationFieldNumbers {
  int timestamp = 1;
  int width = 2;
  int height = 3;
  int distortion_model = 4;
  int D = 5;
  int K = 6;
  int R = 7;
  int P = 8;
  int frame_id = 9;
};
[[nodiscard]] CameraCalibrationFieldNumbers resolveCameraCalibrationFieldNumbers(
    const google::protobuf::Descriptor* descriptor);

/// Field numbers for foxglove.FrameTransform. Defaults = official Foxglove numbering.
struct FrameTransformFieldNumbers {
  int timestamp = 1;
  int parent_frame_id = 2;
  int child_frame_id = 3;
  int translation = 4;
  int rotation = 5;
};
[[nodiscard]] FrameTransformFieldNumbers resolveFrameTransformFieldNumbers(
    const google::protobuf::Descriptor* descriptor);

/// Field numbers for foxglove.ImageAnnotations (top level). Defaults = official.
/// Its nested annotation sub-messages carry no `frame_id`, so their fields are
/// not reached by the converters that renumber frame_id-bearing messages; those
/// nested readers keep the official numbering.
struct ImageAnnotationsFieldNumbers {
  int circles = 1;
  int points = 2;
  int texts = 3;
};
[[nodiscard]] ImageAnnotationsFieldNumbers resolveImageAnnotationsFieldNumbers(
    const google::protobuf::Descriptor* descriptor);

/// Field numbers for foxglove.SceneEntity, the nested message of SceneUpdate.
/// SceneEntity DOES carry a `frame_id`, so it is renumbered by the same files
/// that renumber RawImage/CameraCalibration; resolve it from the nested descriptor.
/// The leaf primitive sub-messages (arrows/cubes/…) carry no frame_id and keep
/// the official numbering. Defaults = official Foxglove numbering.
struct SceneEntityFieldNumbers {
  int timestamp = 1;
  int frame_id = 2;
  int id = 3;
  int lifetime = 4;
  int frame_locked = 5;
  int arrows = 7;
  int cubes = 8;
  int spheres = 9;
  int cylinders = 10;
  int lines = 11;
  int triangles = 12;
  int texts = 13;
  int models = 14;
};

/// Field numbers for foxglove.SceneUpdate: top-level `entities` plus the nested
/// SceneEntity numbering. Defaults = official Foxglove numbering.
struct SceneUpdateFieldNumbers {
  int entities = 2;
  SceneEntityFieldNumbers entity;
};
[[nodiscard]] SceneUpdateFieldNumbers resolveSceneUpdateFieldNumbers(const google::protobuf::Descriptor* descriptor);

/// foxglove.FrameTransform is a SINGLE transform; the SDK object carries a
/// vector, so the result holds exactly one element.
[[nodiscard]] PJ::Expected<PJ::sdk::FrameTransforms> deserializeFoxgloveFrameTransform(
    const uint8_t* data, size_t size, const FrameTransformFieldNumbers& fields = {});

/// Zero-copy: the returned Image's `data` span ALIASES `[data, data+size)` and
/// its `anchor` is set to the supplied anchor (the compressed payload — JPEG/PNG
/// — is never copied). `encoding` is the foxglove `format` string verbatim.
[[nodiscard]] PJ::Expected<PJ::sdk::Image> deserializeFoxgloveCompressedImageView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor, const CompressedImageFieldNumbers& fields = {});

/// foxglove.RawImage -> sdk::Image (UNCOMPRESSED pixels). Zero-copy like the
/// CompressedImage view, but width/height/encoding/row_step are populated from
/// the message so the consumer can interpret the raw pixel bytes (same contract
/// as a ROS sensor_msgs/Image; `encoding` is the foxglove encoding verbatim).
[[nodiscard]] PJ::Expected<PJ::sdk::Image> deserializeFoxgloveRawImageView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor, const RawImageFieldNumbers& fields = {});

[[nodiscard]] PJ::Expected<PJ::sdk::CameraInfo> deserializeFoxgloveCameraCalibration(
    const uint8_t* data, size_t size, const CameraCalibrationFieldNumbers& fields = {});

[[nodiscard]] PJ::Expected<PJ::sdk::ImageAnnotations> deserializeFoxgloveImageAnnotations(
    const uint8_t* data, size_t size, const ImageAnnotationsFieldNumbers& fields = {});

[[nodiscard]] PJ::Expected<PJ::sdk::SceneEntities> deserializeFoxgloveSceneUpdate(
    const uint8_t* data, size_t size, const SceneUpdateFieldNumbers& fields = {});

}  // namespace pj_protobuf
