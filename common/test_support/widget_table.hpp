// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

/// @file widget_table.hpp
/// @brief Reading a dialog's table back from its widget data (PJ::WidgetData JSON): the rows,
///  the selection it shows and a row's index. The caller links nlohmann_json.

#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>

namespace toolbox_testing {

/// The entry of table `widget` in `widget_data_json` (a dialog's widget_data() or
/// PJ::WidgetData::toJson()).
[[nodiscard]] inline nlohmann::json table(std::string_view widget_data_json, const std::string& widget) {
  return nlohmann::json::parse(widget_data_json)[widget];
}

/// The names the table shows selected.
[[nodiscard]] inline std::set<std::string> shownSelection(const nlohmann::json& table) {
  return table.value("selected_items", std::set<std::string>{});
}

/// The index of the row whose first cell is `name`, or -1.
[[nodiscard]] inline int rowOf(const nlohmann::json& table, const std::string& name) {
  const auto rows = table.value("rows", nlohmann::json::array());
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i][0] == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace toolbox_testing
