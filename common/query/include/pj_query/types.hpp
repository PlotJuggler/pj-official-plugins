// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <map>
#include <string>
#include <vector>

namespace PJ::query {

using Metadata = std::map<std::string, std::string, std::less<>>;

// Key -> sorted unique values.
using Schema = std::map<std::string, std::vector<std::string>, std::less<>>;

}  // namespace PJ::query
