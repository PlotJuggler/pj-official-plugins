// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/api.h>

#include <memory>
#include <string>

#include "flight/metadata.hpp"  // mosaico::extractOntologyTag / detectOntologyTag

namespace mosaico {

// True for the image ontologies the toolbox routes into the ObjectStore /
// 2D view (everything else — point clouds, poses, scalars — goes to the
// scalar appendArrowStream path).
[[nodiscard]] inline bool isImageOntology(const std::string& tag) {
  return tag == "image" || tag == "compressed_image";
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
