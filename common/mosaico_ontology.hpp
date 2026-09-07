// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once
#include <pj_base/builtin/builtin_object.hpp>
#include <string>
namespace mosaico {
[[nodiscard]] inline bool isImageOntology(const std::string& tag) {
  return tag == "image" || tag == "compressed_image";
}

[[nodiscard]] inline bool isPointCloudOntology(const std::string& tag) {
  return tag == "point_cloud2" || tag == "point_cloud";
}
[[nodiscard]] inline bool isPoseOntology(const std::string& tag) {
  return tag == "pose" || tag == "motion_state";
}

[[nodiscard]] inline bool isTransformOntology(const std::string& tag) {
  return tag == "transform" || tag == "frame_transform";
}

[[nodiscard]] inline bool isOccupancyGridOntology(const std::string& tag) {
  return tag == "occupancy_grid";
}

[[nodiscard]] inline bool isLaserScanOntology(const std::string& tag) {
  return tag == "laser_scan";
}

[[nodiscard]] inline bool isGridCellsOntology(const std::string& tag) {
  return tag == "grid_cells";
}

[[nodiscard]] inline bool isFuturesPointCloudOntology(const std::string& tag) {
  return tag == "lidar" || tag == "radar" || tag == "rgbd_camera" || tag == "tof_camera" || tag == "stereo_camera";
}

[[nodiscard]] inline bool isCanonicalObjectOntology(const std::string& tag) {
  return isImageOntology(tag) || isPointCloudOntology(tag) || isPoseOntology(tag) || isTransformOntology(tag) ||
         isOccupancyGridOntology(tag) || isLaserScanOntology(tag) || isGridCellsOntology(tag) ||
         isFuturesPointCloudOntology(tag);
}

[[nodiscard]] inline PJ::sdk::BuiltinObjectType objectType(const std::string& tag) {
  using Type = PJ::sdk::BuiltinObjectType;
  if (isImageOntology(tag)) {
    return Type::kImage;
  }
  if (isPointCloudOntology(tag) || isLaserScanOntology(tag) || isFuturesPointCloudOntology(tag)) {
    return Type::kPointCloud;
  }
  if (isPoseOntology(tag)) {
    return Type::kPosesInFrame;
  }
  if (isTransformOntology(tag)) {
    return Type::kFrameTransforms;
  }
  if (isOccupancyGridOntology(tag)) {
    return Type::kOccupancyGrid;
  }
  if (isGridCellsOntology(tag)) {
    return Type::kSceneEntities;
  }
  return Type::kNone;
}
}  // namespace mosaico
