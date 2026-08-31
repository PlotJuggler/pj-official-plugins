// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <pj_base/sdk/plugin_data_api.hpp>

#include "descriptor_import/arrow_cache_artifact.hpp"

namespace mosaico {

/// The cache budget from the host settings: `mosaico/cache_max_gb` (GiB;
/// <= 0 = unlimited), defaulting to kDefaultCacheMaxGb. An advanced setting
/// with no panel UI. Main-thread only (SettingsView).
[[nodiscard]] inline PJ::sdk::descriptor_import::CleanupPolicy cacheCleanupPolicyFromSettings(
    PJ::sdk::SettingsView settings) {
  double max_gb = kDefaultCacheMaxGb;
  if (auto stored = settings.value("mosaico/cache_max_gb")) {
    max_gb = stored->toDouble(kDefaultCacheMaxGb);
  }
  return cacheCleanupPolicy(max_gb);
}

}  // namespace mosaico
