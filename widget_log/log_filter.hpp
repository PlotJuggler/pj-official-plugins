// Row filter of the log viewer: level, node, message text and topic. Qt-free.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "log_store.hpp"

namespace log_viewer {

/// The spellings of a level: `key` in the saved configuration, `label` on screen.
struct LevelName {
  Level level;
  std::string_view key;
  std::string_view label;
};

/// Every level: the ones the user can toggle in toolbar order, then Level::kUnknown, which has no toggle
/// (it is always shown) and no configuration key.
inline constexpr std::array<LevelName, 6> kLevelNames = {{
    {Level::kDebug, "debug", "Debug"},
    {Level::kInfo, "info", "Info"},
    {Level::kWarning, "warn", "Warn"},
    {Level::kError, "error", "Error"},
    {Level::kFatal, "fatal", "Fatal"},
    {Level::kUnknown, "", "Unknown"},
}};

/// Levels the user can toggle, in toolbar order: kLevelNames without kUnknown.
inline constexpr std::array<Level, kLevelNames.size() - 1> kToggleLevels = [] {
  std::array<Level, kLevelNames.size() - 1> levels{};
  for (size_t i = 0; i < levels.size(); ++i) {
    levels[i] = kLevelNames[i].level;
  }
  return levels;
}();

struct LogFilter {
  /// Indexed by the Level value; kUnknown is ignored (always shown).
  std::array<bool, 6> levels = {true, true, true, true, true, true};
  /// Node name to keep; empty = every node.
  std::string node;
  /// Case-insensitive (ASCII) substring of the message; empty = every message.
  std::string text;
  /// Wire ids whose rows are hidden.
  std::vector<uint32_t> hidden_wires;
};

/// A LogFilter resolved against one store, cheap to evaluate per row.
class CompiledFilter {
 public:
  CompiledFilter(const LogFilter& filter, const LogStore& store);

  [[nodiscard]] bool accepts(const LogRow& row) const;

  /// Append to @p out the indices of the rows in [@p from, @p to) that pass.
  void collect(const LogStore& store, size_t from, size_t to, std::vector<uint32_t>& out) const;

 private:
  std::array<bool, 6> levels_;
  bool any_node_ = true;
  std::optional<uint32_t> node_;  ///< nullopt with any_node_ false: the node was never seen, nothing passes
  std::string needle_;            ///< lower-cased
  std::vector<uint32_t> hidden_wires_;
};

/// Whether @p haystack contains @p lower_needle, comparing ASCII letters case-insensitively.
bool containsCaseInsensitive(const std::string& haystack, const std::string& lower_needle);

/// Number of entries of @p filtered (store indices in ascending order) whose row time is <= @p tracker.
size_t revealedCount(const LogStore& store, const std::vector<uint32_t>& filtered, double tracker);

}  // namespace log_viewer
