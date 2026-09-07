// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/table.h>

#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/time_math.hpp>

namespace mosaico {

struct MosaicoObjectOptions {
  std::string ontology_tag;
  std::optional<std::string> ts_field;  ///< An empty field name is distinct from no axis.
  PJ::Timestamp fallback_timestamp = 0;
  PJ::TimeUnit timestamp_unit = PJ::TimeUnit::kNanoseconds;
  PJ::sdk::BufferAnchor payload_anchor;
};

[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>> flattenStructColumns(std::shared_ptr<arrow::Table> table);

/// Decode one source row; owned geometry and borrowed media both outlive the input call.
[[nodiscard]] PJ::Expected<PJ::sdk::ObjectRecord> decodeMosaicoObject(
    const MosaicoObjectOptions& options, const std::shared_ptr<arrow::Table>& table);

}  // namespace mosaico
