// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_host_views.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pj::streaming {

enum class DelegatedIngestDisposition {
  kPushed,
  kBindingUnavailable,
};

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
/// the host ABI. A binding lookup failure is returned as a non-error disposition
/// so sources preserve their historical skip-and-retry behavior; only a failed
/// push is an error subject to the source's existing error policy.
class DelegatedIngestCache {
 public:
  [[nodiscard]] PJ::Expected<DelegatedIngestDisposition> push(
      const PJ::DataSourceRuntimeHostView& host, std::string_view cache_key, const PJ::ParserBindingRequest& request,
      PJ::Timestamp timestamp, std::vector<uint8_t> payload) {
    auto binding = bindings_.find(cache_key);
    if (binding == bindings_.end()) {
      auto created = host.ensureParserBinding(request);
      if (!created) {
        return DelegatedIngestDisposition::kBindingUnavailable;
      }
      binding = bindings_.emplace(std::string(cache_key), *created).first;
    }

    auto owned = std::make_shared<std::vector<uint8_t>>(std::move(payload));
    auto status = host.pushMessage(
        binding->second, timestamp, [owned]() -> PJ::sdk::PayloadView { return PJ::sdk::PayloadView{owned}; });
    if (!status) {
      return PJ::unexpected(status.error());
    }
    return DelegatedIngestDisposition::kPushed;
  }

  void clear() {
    bindings_.clear();
  }

 private:
  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }

    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
      return (*this)(std::string_view(value));
    }
  };

  std::unordered_map<std::string, PJ::ParserBindingHandle, TransparentStringHash, std::equal_to<>> bindings_;
};

}  // namespace pj::streaming
