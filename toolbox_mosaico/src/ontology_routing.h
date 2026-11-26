// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/api.h>

#include <memory>
#include <string>

#include "../../common/mosaico_ontology.hpp"
#include "flight/metadata.hpp"  // mosaico::extractOntologyTag / detectOntologyTag

namespace mosaico {

// Resolve a pulled topic's ontology tag from the server metadata (mosaico:properties -> ontology_tag), the single
// source of truth. There is no column-structure guessing: a `data` binary column + width/height also matches point
// clouds (ROS PointCloud2) and geometry-less compressed images. Order:
//   1) the tag cached from getTopicMetadata (extractOntologyTag on the topic),
//   2) extractOntologyTag on the pulled stream's schema metadata.
// An absent tag yields "": the topic is then plain scalar data.
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
