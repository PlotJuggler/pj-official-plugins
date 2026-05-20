// Pure naming logic for flattening LeRobot vector features
// (observation.state / action stored as list<float>[K]) into K scalar series.
// No Arrow, no host APIs — unit-testable in isolation.
#pragma once

#include <string>
#include <vector>

namespace lerobot {

/// Labels for the K scalar columns a width-K vector feature expands into.
/// `base` is the feature name (e.g. "observation.state"); if `names` has at
/// least K entries the i-th label is `base + "." + names[i]`, otherwise
/// `base + "." + i`. K <= 0 yields an empty vector.
[[nodiscard]] std::vector<std::string> flattenedFieldNames(
    const std::string& base, int k, const std::vector<std::string>& names);

/// Disambiguate duplicate names in-place order: the first occurrence keeps its
/// name, later duplicates get a `__2`, `__3`, … suffix so no series silently
/// overwrites another. Returns the deduplicated list (same size/order).
[[nodiscard]] std::vector<std::string> dedupeFieldNames(const std::vector<std::string>& names);

}  // namespace lerobot
