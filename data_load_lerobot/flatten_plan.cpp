#include "flatten_plan.hpp"

#include <string>
#include <unordered_map>

namespace lerobot {

std::vector<std::string> flattenedFieldNames(const std::string& base, int k, const std::vector<std::string>& names) {
  std::vector<std::string> out;
  if (k <= 0) {
    return out;
  }
  out.reserve(static_cast<std::size_t>(k));
  for (int i = 0; i < k; ++i) {
    const auto idx = static_cast<std::size_t>(i);
    if (idx < names.size() && !names[idx].empty()) {
      out.push_back(base + "." + names[idx]);
    } else {
      out.push_back(base + "." + std::to_string(i));
    }
  }
  return out;
}

std::vector<std::string> dedupeFieldNames(const std::vector<std::string>& names) {
  std::vector<std::string> out;
  out.reserve(names.size());
  std::unordered_map<std::string, int> seen;
  for (const auto& n : names) {
    const int count = ++seen[n];
    if (count == 1) {
      out.push_back(n);
    } else {
      out.push_back(n + "__" + std::to_string(count));
    }
  }
  return out;
}

}  // namespace lerobot
