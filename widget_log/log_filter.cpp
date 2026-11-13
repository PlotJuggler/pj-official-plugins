#include "log_filter.hpp"

#include <algorithm>

namespace log_viewer {

namespace {

char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Lower-cased exactly as containsCaseInsensitive() folds the haystack: std::tolower follows the C locale,
/// which Qt sets from the environment, and could fold UTF-8 bytes the haystack keeps.
std::string lowerNeedle(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), lowerAscii);
  return text;
}

}  // namespace

bool containsCaseInsensitive(const std::string& haystack, const std::string& lower_needle) {
  if (lower_needle.empty()) {
    return true;
  }
  const auto found = std::search(
      haystack.begin(), haystack.end(), lower_needle.begin(), lower_needle.end(),
      [](char a, char b) { return lowerAscii(a) == b; });
  return found != haystack.end();
}

CompiledFilter::CompiledFilter(const LogFilter& filter, const LogStore& store)
    : levels_(filter.levels), needle_(lowerNeedle(filter.text)), hidden_wires_(filter.hidden_wires) {
  levels_[static_cast<size_t>(Level::kUnknown)] = true;
  if (!filter.node.empty()) {
    any_node_ = false;
    node_ = store.find(filter.node);
  }
  std::sort(hidden_wires_.begin(), hidden_wires_.end());
}

bool CompiledFilter::accepts(const LogRow& row) const {
  const auto level = static_cast<size_t>(row.level);
  if (level < levels_.size() && !levels_[level]) {
    return false;
  }
  if (!any_node_ && (!node_ || row.node != *node_)) {
    return false;
  }
  if (!hidden_wires_.empty() && std::binary_search(hidden_wires_.begin(), hidden_wires_.end(), row.wire)) {
    return false;
  }
  return containsCaseInsensitive(row.message, needle_);
}

void CompiledFilter::collect(const LogStore& store, size_t from, size_t to, std::vector<uint32_t>& out) const {
  for (size_t i = from; i < to; ++i) {
    if (accepts(store.row(i))) {
      out.push_back(static_cast<uint32_t>(i));
    }
  }
}

size_t revealedCount(const LogStore& store, const std::vector<uint32_t>& filtered, double tracker) {
  const auto end = std::upper_bound(filtered.begin(), filtered.end(), tracker, [&store](double t, uint32_t index) {
    return t < store.row(index).time;
  });
  return static_cast<size_t>(std::distance(filtered.begin(), end));
}

}  // namespace log_viewer
