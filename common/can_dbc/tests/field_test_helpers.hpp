#pragma once

// Finds one field by name in the span SignalRowBuilder::build() returns; shared by pj_can_dbc's
// tests and data_load_blf/data_load_candump's.

#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/span.hpp>
#include <string>

namespace pj_can_dbc::testing {

inline const PJ::sdk::NamedFieldValue* findField(
    const PJ::Span<const PJ::sdk::NamedFieldValue>& fields, const std::string& name) {
  for (const auto& field : fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

}  // namespace pj_can_dbc::testing
