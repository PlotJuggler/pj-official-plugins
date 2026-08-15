// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/api.h>

#include <memory>
#include <string>

#include "flight/metadata.hpp"  // mosaico::extractOntologyTag / detectOntologyTag
#include "image_metadata.hpp"
#include "object_metadata.hpp"

namespace mosaico {

// True for the image ontologies the toolbox routes into the ObjectStore /
// 2D view.
[[nodiscard]] inline bool isImageOntology(const std::string& tag) {
  return tag == "image" || tag == "compressed_image";
}

// The 3D-object ontologies the toolbox canonicalizes into a pj_base builtin
// object (sdk::PointCloud / PosesInFrame) for the pj_scene3D consumer to render.
// The HOST decodes; the toolbox only repackages (the server ships these decoded
// — point clouds as ROS-PointCloud2 packed `data`+`fields`, poses as scalar
// struct columns — so there is nothing for the plugin to decompress).
//
// Tags are the REAL ones the demo server emits (verified live against
// demo.mosaico.dev): `point_cloud2` for clouds; `pose` and `motion_state`
// (odometry) for poses. (`point_cloud` is accepted as a forward-compat alias for
// the canonical-named ontology, though the live server uses `point_cloud2`.) The
// server has NO transform/tf ontology, so TF is not sourced here. Anything not
// matched (and not an image) falls through to the scalar appendArrowStream path.
[[nodiscard]] inline bool isPointCloudOntology(const std::string& tag) {
  return tag == "point_cloud2" || tag == "point_cloud";
}
[[nodiscard]] inline bool isPoseOntology(const std::string& tag) {
  return tag == "pose" || tag == "motion_state";
}

// TF-style frame relationships → sdk::FrameTransforms (kFrameTransforms). The
// Mosaico `transform` ontology carries one transform per row (translation +
// rotation + target_frame_id, parent frame in the header); `frame_transform`
// carries a `transforms` list of those per row. The live demo server emits no
// TF ontology today, so this is forward-compatible coverage matching the
// Python SDK's tf2_msgs/TFMessage → frame_transform mapping.
[[nodiscard]] inline bool isTransformOntology(const std::string& tag) {
  return tag == "transform" || tag == "frame_transform";
}

// 2D metric maps / costmaps → sdk::OccupancyGrid (kOccupancyGrid), the same
// builtin the ROS/MCAP path uses for nav_msgs/OccupancyGrid (rendered by
// pj_scene3D's OccupancyGridLayer, costmap color scheme included).
[[nodiscard]] inline bool isOccupancyGridOntology(const std::string& tag) {
  return tag == "occupancy_grid";
}

// Polar laser scans → sdk::PointCloud (kPointCloud). The converter expands the
// polar `ranges` list into packed XYZ points (one per beam).
[[nodiscard]] inline bool isLaserScanOntology(const std::string& tag) {
  return tag == "laser_scan";
}

// Sparse grid-cell sets → sdk::SceneEntities (kSceneEntities), one flat cube
// per marked cell (sized by cell_width/cell_height). Distinct from the dense
// occupancy_grid raster above; mirrors how the ROS path renders sparse 3D
// primitives (markers) as SceneEntities.
[[nodiscard]] inline bool isGridCellsOntology(const std::string& tag) {
  return tag == "grid_cells";
}

// Experimental Mosaico "futures" sensor clouds → sdk::PointCloud (kPointCloud).
// Unlike `point_cloud2` (a single packed `data` buffer), these arrive COLUMNAR:
// one Arrow list column per per-point attribute (x/y/z + intensity, doppler,
// rgb, …). The converter packs the parallel lists into a canonical PointCloud
// buffer. These tags are experimental in the Python SDK and may still churn.
[[nodiscard]] inline bool isFuturesPointCloudOntology(const std::string& tag) {
  return tag == "lidar" || tag == "radar" || tag == "rgbd_camera" || tag == "tof_camera" || tag == "stereo_camera";
}

// True for any ontology the toolbox canonicalizes into a pj_base builtin object
// (image + the 3D types) rather than routing to the scalar pipeline.
[[nodiscard]] inline bool isCanonicalObjectOntology(const std::string& tag) {
  return isImageOntology(tag) || isPointCloudOntology(tag) || isPoseOntology(tag) || isTransformOntology(tag) ||
         isOccupancyGridOntology(tag) || isLaserScanOntology(tag) || isGridCellsOntology(tag) ||
         isFuturesPointCloudOntology(tag);
}

// Resolve a pulled topic's ontology tag from the server metadata — the single
// source of truth. Mosaico tags every topic in its Arrow schema
// (mosaico:properties → ontology_tag), so there is NO column-structure
// guessing here: an earlier heuristic that inferred "image" from a `data`
// binary column + width/height false-positived on point clouds (ROS
// PointCloud2 carries the same columns) and on geometry-less compressed
// images. We read the tag explicitly instead:
//   1) the tag cached from getTopicMetadata (extractOntologyTag on the topic),
//   2) extractOntologyTag on the pulled stream's schema metadata.
// An absent tag yields "" — the topic is then treated as plain scalar data
// rather than guessed at.
/// The canonical registerObjectTopic metadata for a resolved ontology tag —
/// the SAME record the per-ontology push helpers register on the host, kept
/// here next to the routing predicates so the cache artifact's channel
/// metadata can never drift from the ingest routing. Cloud-like tags (point
/// cloud / laser scan / futures cloud) share the point-cloud record.
[[nodiscard]] inline std::string_view canonicalMetadataForOntology(const std::string& tag) {
  if (isImageOntology(tag)) {
    return kCanonicalImageMetadata;
  }
  if (isPoseOntology(tag)) {
    return kCanonicalPosesInFrameMetadata;
  }
  if (isTransformOntology(tag)) {
    return kCanonicalFrameTransformsMetadata;
  }
  if (isOccupancyGridOntology(tag)) {
    return kCanonicalOccupancyGridMetadata;
  }
  if (isGridCellsOntology(tag)) {
    return kCanonicalSceneEntitiesMetadata;
  }
  return kCanonicalPointCloudMetadata;
}

[[nodiscard]] inline std::string resolveOntologyTag(
    const std::shared_ptr<arrow::Schema>& schema, const std::string& cached_tag) {
  if (!cached_tag.empty()) {
    return cached_tag;
  }
  if (schema) {
    if (const auto& md = schema->metadata()) {
      if (auto tag = extractOntologyTag(std::const_pointer_cast<arrow::KeyValueMetadata>(md))) {
        return *tag;
      }
    }
  }
  return {};
}

}  // namespace mosaico
