// Naming for flattening LeRobot vector features (observation.state / action stored as list<float>[K]) into K scalar
// series.
#pragma once

#include <string>
#include <vector>

namespace lerobot {

/// Labels for the K scalar columns of a width-K vector feature `base` (e.g. "observation.state"): the i-th is
/// `base.names[i]`, or `base.i` when `names` has no non-empty i-th entry. K <= 0 yields an empty vector.
[[nodiscard]] std::vector<std::string> flattenedFieldNames(
    const std::string& base, int k, const std::vector<std::string>& names);

/// The first occurrence keeps its name, later duplicates get a `__2`, `__3`, ... suffix so no series silently
/// overwrites another. Same size/order as the input.
[[nodiscard]] std::vector<std::string> dedupeFieldNames(const std::vector<std::string>& names);

}  // namespace lerobot
