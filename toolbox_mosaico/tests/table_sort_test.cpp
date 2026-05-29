// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Unit tests for the column-sort permutation that backs sortable sequence /
// topic tables. The key property: numeric columns (Date, Size) sort
// numerically, NOT lexicographically — which is exactly why the plugin owns
// sorting instead of letting QTableWidget compare display strings ("100 MB"
// would otherwise sort before "20 MB").

#include "../src/table_sort.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

using mosaico::sortedPermutation;

TEST(TableSort, NumericAscending) {
  std::vector<std::int64_t> sizes = {100, 20, 300, 5};
  auto perm = sortedPermutation(sizes, /*ascending=*/true);
  // Expect order of indices: 5(3), 20(1), 100(0), 300(2).
  EXPECT_EQ(perm, (std::vector<std::size_t>{3, 1, 0, 2}));
}

TEST(TableSort, NumericDescending) {
  std::vector<std::int64_t> sizes = {100, 20, 300, 5};
  auto perm = sortedPermutation(sizes, /*ascending=*/false);
  EXPECT_EQ(perm, (std::vector<std::size_t>{2, 0, 1, 3}));
}

TEST(TableSort, StringSortIsNotLexicographicOverNumbers) {
  // Strings sort lexicographically (this column is "Name", not a number).
  std::vector<std::string> names = {"beta", "Alpha", "gamma"};
  auto perm = sortedPermutation(names, /*ascending=*/true);
  // ASCII: 'A'(0x41) < 'b'(0x62) < 'g'. So "Alpha", "beta", "gamma".
  EXPECT_EQ(perm, (std::vector<std::size_t>{1, 0, 2}));
}

TEST(TableSort, StableForEqualKeys) {
  // Equal keys must keep original relative order (PJ3 has no secondary sort).
  std::vector<std::int64_t> keys = {7, 7, 3, 7, 3};
  auto perm = sortedPermutation(keys, /*ascending=*/true);
  // The two 3s (indices 2,4) come first in original order, then the 7s (0,1,3).
  EXPECT_EQ(perm, (std::vector<std::size_t>{2, 4, 0, 1, 3}));
}

TEST(TableSort, EmptyInput) {
  std::vector<std::int64_t> empty;
  EXPECT_TRUE(sortedPermutation(empty, true).empty());
}

TEST(TableSort, SingleElement) {
  std::vector<std::string> one = {"only"};
  EXPECT_EQ(sortedPermutation(one, false), (std::vector<std::size_t>{0}));
}
