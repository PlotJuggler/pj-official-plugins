// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

namespace mosaico {

// Stable sort permutation for one table column. `keys[i]` is the comparable
// value for row i (std::string for Name columns, std::int64_t for Date/Size).
// Returns row indices in ascending or descending order. Stable: equal keys
// keep their original relative order (PJ3 sortable-column parity, where a
// secondary sort isn't applied). Numeric keys sort numerically, never
// lexicographically — the reason the plugin owns sorting instead of relying
// on table widgets to compare display strings.
template <typename Key>
std::vector<std::size_t> sortedPermutation(const std::vector<Key>& keys, bool ascending) {
  std::vector<std::size_t> perm(keys.size());
  std::iota(perm.begin(), perm.end(), std::size_t{0});
  std::stable_sort(perm.begin(), perm.end(), [&](std::size_t a, std::size_t b) {
    if (keys[a] == keys[b]) {
      return false;  // preserve original order for equal keys (stable)
    }
    return ascending ? (keys[a] < keys[b]) : (keys[b] < keys[a]);
  });
  return perm;
}

}  // namespace mosaico
