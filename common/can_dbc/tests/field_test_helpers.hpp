#pragma once

// Shared by pj_can_dbc's own tests and by data_load_blf/data_load_candump's
// tests that build rows through SignalRowBuilder: finds one field by name in
// the flat PJ::sdk::NamedFieldValue span build() returns, instead of each
// test file writing its own linear-scan loop.

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
