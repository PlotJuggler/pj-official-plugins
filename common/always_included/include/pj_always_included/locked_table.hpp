// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

/// @file locked_table.hpp
/// @brief The topic table of a streamer's dialog (PlotJuggler Bridge, Foxglove Bridge, ROS 2),
///  whose always-included rows are locked (the rule is in always_included.hpp), so every such
///  dialog behaves the same. `rows` are the rows the dialog's filter shows, which never hides a
///  locked row. A selection is a set of topic names (column 0), the one the dialog saves; locked
///  rows are added only when the table is rendered.

#include <pj_always_included/always_included.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PJ::AlwaysIncluded {

struct TableRow {
  std::vector<std::string> cells;  ///< cells[0] is the topic name, the row's identity
  bool locked = false;             ///< always included
};
using TableRows = std::vector<TableRow>;

/// Fills `table` with `rows`, the locked ones disabled with kTooltip, and selects `selection`
/// plus every locked row. Also sets the table's placeholder and the shortcuts of the dialog's
/// btnSelectAll (Ctrl+A) and btnDeselectAll (Ctrl+Shift+A). The host matches selected items to
/// rows by first-column text, so a selection the filter hides is harmless and the selection
/// survives a user sort of the table.
inline void tableToWidgets(
    PJ::WidgetData& wd, std::string_view table, TableRows rows, const std::set<std::string>& selection) {
  std::vector<std::vector<std::string>> cells;
  std::vector<int> locked;
  std::vector<std::string> selected(selection.begin(), selection.end());
  cells.reserve(rows.size());
  for (auto& row : rows) {
    if (row.locked) {
      const int index = static_cast<int>(cells.size());
      locked.push_back(index);
      wd.setCellTooltip(table, index, 0, kTooltip);
      if (!selection.contains(row.cells.front())) {
        selected.push_back(row.cells.front());
      }
    }
    cells.push_back(std::move(row.cells));
  }
  wd.setTableRows(table, cells);
  wd.setDisabledRows(table, locked);
  wd.setSelectedItems(table, selected);
  wd.setListPlaceholder(table, "Click to select, Click+Drag to multi-select");
  wd.setShortcut("btnSelectAll", "Ctrl+A");
  wd.setShortcut("btnDeselectAll", "Ctrl+Shift+A");
}

/// Applies a click on btnSelectAll (adds every shown unlocked row; a selection the filter hides
/// stays) or btnDeselectAll (drops every selection, also one the filter hides). False for
/// another widget.
inline bool applySelectButton(std::string_view widget_name, std::set<std::string>& selection, const TableRows& rows) {
  if (widget_name == "btnSelectAll") {
    for (const auto& row : rows) {
      if (!row.locked) {
        selection.insert(row.cells.front());
      }
    }
    return true;
  }
  if (widget_name == "btnDeselectAll") {
    selection.clear();
    return true;
  }
  return false;
}

/// Applies the selection the host reported. The host reports only the rows it shows, so a
/// selection the filter hides stays; a locked row it reports is not added.
inline void applyReported(
    std::set<std::string>& selection, const std::vector<std::string>& reported, const TableRows& rows) {
  std::set<std::string_view> locked;
  for (const auto& row : rows) {
    selection.erase(row.cells.front());
    if (row.locked) {
      locked.insert(row.cells.front());
    }
  }
  for (const auto& name : reported) {
    if (!locked.contains(name)) {
      selection.insert(name);
    }
  }
}

}  // namespace PJ::AlwaysIncluded
