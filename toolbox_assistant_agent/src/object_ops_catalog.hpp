// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once
//
// The Luau operations a script (create_derived_series/evaluate) may call on a
// derived-object BuiltinObject of a given type, advertised by describe_topic
// alongside the type's field table. This table mirrors PJ4's
// pj_scripting/src/object_binder.cpp -- the generic Luau <-> sdk::BuiltinObject
// binder that actually exposes these calls to a running script -- so the
// assistant never advertises a method a script cannot really call. A type with
// no entry here (FrameTransforms, ImageAnnotations, and every type with no
// field table at all) exposes its field table only; there is nothing to add.
//
// KNOWN GAP: object_binder.cpp does not yet bind a coordinate-transform method
// on PointCloud (no "transform(tf)"), even though earlier design notes
// mention one. This table lists only what a script can actually call today.

#include <pj_base/builtin/builtin_object.hpp>
#include <string_view>
#include <vector>

namespace assistant_agent {

// One callable operation: its Luau call shape and a one-line description.
struct ObjectOperation {
  std::string_view name;
  std::string_view doc;
};

// The operations a script may call on an object of `type`, or empty when the
// type exposes only its field table (see the file comment).
[[nodiscard]] inline std::vector<ObjectOperation> objectOperationsFor(PJ::sdk::BuiltinObjectType type) {
  using PJ::sdk::BuiltinObjectType;
  switch (type) {
    case BuiltinObjectType::kPointCloud:
      return {
          {"count()", "number of points in the cloud"},
          {"bounds()", "axis-aligned {min={x,y,z}, max={x,y,z}, count} over finite points, or nil if empty"},
          {"crop_box{min={x,y,z}, max={x,y,z}}", "new cloud keeping only points inside the box"},
          {"filter(channel, op, value)",
           "new cloud keeping points where channel <op> value; op is one of <, <=, >, >=, ==, ~="},
          {"extreme_point(channel, \"max\"|\"min\")", "the point with the extreme value of channel, or nil if empty"},
          {"xyz_points()", "iterator over the cloud's finite {x, y, z} points"},
      };
    case BuiltinObjectType::kSceneEntities:
      return {
          {"pj.scene.new{frame_id=, id=}:sphere{center={x,y,z},diameter=,color={r,g,b,a}?}"
           ":cube{center={x,y,z},size={x,y,z},color={r,g,b,a}?}"
           ":line_strip{points={{x,y,z},...},thickness=,color={r,g,b,a}?}:finish()",
           "builder: chain shape calls off pj.scene.new(...), then :finish() to materialize a SceneEntities object"},
      };
    case BuiltinObjectType::kFrameTransforms:
    case BuiltinObjectType::kImageAnnotations:
    case BuiltinObjectType::kNone:
    case BuiltinObjectType::kImage:
    case BuiltinObjectType::kDepthImage:
    case BuiltinObjectType::kOccupancyGrid:
    case BuiltinObjectType::kCompressedPointCloud:
    case BuiltinObjectType::kMesh3D:
    case BuiltinObjectType::kVideoFrame:
    case BuiltinObjectType::kRobotDescription:
    case BuiltinObjectType::kCameraInfo:
    case BuiltinObjectType::kOccupancyGridUpdate:
    case BuiltinObjectType::kLog:
    case BuiltinObjectType::kPosesInFrame:
    case BuiltinObjectType::kVoxelGrid:
    case BuiltinObjectType::kPlotMarkers:
    case BuiltinObjectType::kGridMap:
      return {};
  }
  return {};
}

}  // namespace assistant_agent
