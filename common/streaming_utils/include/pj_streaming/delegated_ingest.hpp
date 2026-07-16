// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_host_views.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pj::streaming {

/// Safely read the parser-specific config injected by the host. Wrong-typed
/// values degrade to empty instead of throwing out of a plugin load callback.
[[nodiscard]] inline std::string parserConfigOverride(std::string_view config_json) {
  const auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded() || !cfg.is_object()) {
    return {};
  }
  const auto it = cfg.find("_parser_config");
  return it != cfg.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// Cache delegated-ingest parser bindings and anchor owned payload bytes across
/// the host ABI. Protocol sources remain responsible for their error policy.
class DelegatedIngestCache {
 public:
  [[nodiscard]] PJ::Status push(
      const PJ::DataSourceRuntimeHostView& host, std::string cache_key, const PJ::ParserBindingRequest& request,
      PJ::Timestamp timestamp, std::vector<uint8_t> payload) {
    auto binding = bindings_.find(cache_key);
    if (binding == bindings_.end()) {
      auto created = host.ensureParserBinding(request);
      if (!created) {
        return PJ::unexpected(created.error());
      }
      binding = bindings_.emplace(std::move(cache_key), *created).first;
    }

    auto owned = std::make_shared<std::vector<uint8_t>>(std::move(payload));
    return host.pushMessage(
        binding->second, timestamp, [owned]() -> PJ::sdk::PayloadView { return PJ::sdk::PayloadView{owned}; });
  }

  void clear() {
    bindings_.clear();
  }

 private:
  std::unordered_map<std::string, PJ::ParserBindingHandle> bindings_;
};

}  // namespace pj::streaming
