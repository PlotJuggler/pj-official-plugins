// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/api.h>

#include <memory>
#include <string>

#include "flight/metadata.hpp"  // mosaico::extractOntologyTag / detectOntologyTag

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

// True for any ontology the toolbox canonicalizes into a pj_base builtin object
// (image + the 3D types) rather than routing to the scalar pipeline.
[[nodiscard]] inline bool isCanonicalObjectOntology(const std::string& tag) {
  return isImageOntology(tag) || isPointCloudOntology(tag) || isPoseOntology(tag);
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
