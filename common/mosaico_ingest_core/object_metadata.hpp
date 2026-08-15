// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The producer half of the canonical 3D-object topic metadata contract. Each
// constant is the EXACT topic-level metadata_json the PJ4 host keys on to route
// an ObjectStore topic to its pj_scene3D consumer and to pick the canonical
// pj_base codec when no MessageParser is bound (the toolbox push path binds
// none — see pj_scene3d_widgets/resolve_object.{h,cpp}, which decodes the blob
// host-side via the codec selected by `builtin_object_type`).
//
// Only the type tag is needed: unlike the legacy image metadata there is no
// codec-id key — the host dispatches the canonical decode purely on
// `builtin_object_type` (the value is the canonical name from
// PJ::sdk::name(BuiltinObjectType), e.g. "kPointCloud").
//
// Per-frame geometry/frame_id/timestamps live INSIDE each serialized blob (the
// canonical pj_base struct), never in this topic-level metadata.

#pragma once

#include <string_view>

namespace mosaico {

// sdk::PointCloud — packed point cloud. The Mosaico `point_cloud2` ontology is
// ROS PointCloud2-shaped (packed `data` + `fields` descriptor), which IS the
// canonical layout, so the toolbox copies it through; it never decodes.
inline constexpr std::string_view kCanonicalPointCloudMetadata = R"({"builtin_object_type":"kPointCloud"})";

// sdk::PosesInFrame — pose array in one frame. The Mosaico `pose` and
// `motion_state` (odometry) ontologies.
inline constexpr std::string_view kCanonicalPosesInFrameMetadata = R"({"builtin_object_type":"kPosesInFrame"})";

// sdk::FrameTransforms — TF-style frame relationships. The Mosaico `transform`
// and `frame_transform` ontologies.
inline constexpr std::string_view kCanonicalFrameTransformsMetadata = R"({"builtin_object_type":"kFrameTransforms"})";

// sdk::OccupancyGrid — 2D metric map/costmap. The Mosaico `occupancy_grid`
// ontology (same builtin the ROS/MCAP path uses for nav_msgs/OccupancyGrid).
inline constexpr std::string_view kCanonicalOccupancyGridMetadata = R"({"builtin_object_type":"kOccupancyGrid"})";

// sdk::SceneEntities — procedural 3D primitives. The Mosaico `grid_cells`
// ontology renders each marked cell as one flat cube.
inline constexpr std::string_view kCanonicalSceneEntitiesMetadata = R"({"builtin_object_type":"kSceneEntities"})";

}  // namespace mosaico
