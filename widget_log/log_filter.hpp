// Row filter of the log viewer: level, node, message text and topic. Qt-free.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "log_store.hpp"

namespace log_viewer {

/// Levels the user can toggle, in toolbar order. Level::kUnknown has no toggle and is always shown.
inline constexpr std::array<Level, 5> kToggleLevels = {
    Level::kDebug, Level::kInfo, Level::kWarning, Level::kError, Level::kFatal};

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
