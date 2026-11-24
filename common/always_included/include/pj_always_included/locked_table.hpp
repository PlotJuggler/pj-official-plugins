// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

/// @file locked_table.hpp
/// @brief The topic table of a streamer's dialog (PlotJuggler Bridge, Foxglove Bridge, ROS 2),
///  whose always-included rows are locked, so every such dialog behaves the same:
///   - a locked row is shown selected, disabled and carries kTooltip;
///   - Select All, Deselect All and a host-reported selection never drop a locked row.
///  `rows` are the rows the dialog's filter shows, which a filter never hides a locked row from,
///  so they hold every locked topic the dialog knows. A selection is a set of topic names
///  (column 0); it may lack the locked ones: withLocked() is what the dialog shows and saves.

#include <algorithm>
#include <pj_always_included/always_included.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace PJ::AlwaysIncluded {

struct TableRow {
  std::vector<std::string> cells;  ///< cells[0] is the topic name, the row's identity
  bool locked = false;             ///< always included
};
using TableRows = std::vector<TableRow>;

/// `selection` plus every locked row: what the dialog shows selected and saves.
[[nodiscard]] inline std::set<std::string> withLocked(std::set<std::string> selection, const TableRows& rows) {
  for (const auto& row : rows) {
    if (row.locked) {
      selection.insert(row.cells.front());
    }
  }
  return selection;
}

/// Fills `table` with `rows`, the locked ones disabled with kTooltip, and selects withLocked().
/// The host matches selected items to rows by first-column text, so a selection the filter
/// hides is harmless and the selection survives a user sort of the table.
inline void tableToWidgets(
    PJ::WidgetData& wd, std::string_view table, const TableRows& rows, const std::set<std::string>& selection) {
  std::vector<std::vector<std::string>> cells;
  std::vector<int> locked;
  cells.reserve(rows.size());
  for (const auto& row : rows) {
    if (row.locked) {
      const int index = static_cast<int>(cells.size());
      locked.push_back(index);
      wd.setCellTooltip(table, index, 0, kTooltip);
    }
    cells.push_back(row.cells);
  }
  wd.setTableRows(table, cells);
  wd.setDisabledRows(table, locked);
  const auto shown = withLocked(selection, rows);
  wd.setSelectedItems(table, std::vector<std::string>(shown.begin(), shown.end()));
}

/// Select All: adds every shown row; a selection the filter hides stays.
inline void selectAll(std::set<std::string>& selection, const TableRows& rows) {
  for (const auto& row : rows) {
    if (!row.locked) {
      selection.insert(row.cells.front());
    }
  }
}

/// Deselect All: drops every selection, also one the filter hides; locked rows stay selected.
inline void deselectAll(std::set<std::string>& selection, const TableRows& rows) {
  const auto locked = withLocked({}, rows);
  std::erase_if(selection, [&](const std::string& name) { return !locked.contains(name); });
}

/// Applies the selection the host reported. The host reports only the rows it shows, so a
/// selection the filter hides stays; a locked row it reports or omits changes nothing.
inline void applyReported(
    std::set<std::string>& selection, const std::vector<std::string>& reported, const TableRows& rows) {
  std::set<std::string_view> shown;
  for (const auto& row : rows) {
    shown.insert(row.cells.front());
  }
  const auto locked = withLocked({}, rows);
  const auto merged = PJ::sdk::mergeVisibleSelection(
      std::vector<std::string>(selection.begin(), selection.end()), reported,
      [&](const std::string& name) { return shown.contains(name); },
      [&](const std::string& name) { return !locked.contains(name); });
  selection = std::set<std::string>(merged.begin(), merged.end());
}

}  // namespace PJ::AlwaysIncluded
