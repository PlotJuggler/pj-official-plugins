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

namespace pj_protobuf {

/// foxglove.FrameTransform is a SINGLE transform; the SDK object carries a
/// vector, so the result holds exactly one element.
[[nodiscard]] PJ::Expected<PJ::sdk::FrameTransforms> deserializeFoxgloveFrameTransform(
    const uint8_t* data, size_t size);

/// Zero-copy: the returned Image's `data` span ALIASES `[data, data+size)` and
/// its `anchor` is set to the supplied anchor (the compressed payload — JPEG/PNG
/// — is never copied). `encoding` is the foxglove `format` string verbatim.
[[nodiscard]] PJ::Expected<PJ::sdk::Image> deserializeFoxgloveCompressedImageView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor);

/// foxglove.RawImage -> sdk::Image (UNCOMPRESSED pixels). Zero-copy like the
/// CompressedImage view, but width/height/encoding/row_step are populated from
/// the message so the consumer can interpret the raw pixel bytes (same contract
/// as a ROS sensor_msgs/Image; `encoding` is the foxglove encoding verbatim).
[[nodiscard]] PJ::Expected<PJ::sdk::Image> deserializeFoxgloveRawImageView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor);

[[nodiscard]] PJ::Expected<PJ::sdk::CameraInfo> deserializeFoxgloveCameraCalibration(const uint8_t* data, size_t size);

[[nodiscard]] PJ::Expected<PJ::sdk::ImageAnnotations> deserializeFoxgloveImageAnnotations(
    const uint8_t* data, size_t size);

[[nodiscard]] PJ::Expected<PJ::sdk::SceneEntities> deserializeFoxgloveSceneUpdate(const uint8_t* data, size_t size);

}  // namespace pj_protobuf
