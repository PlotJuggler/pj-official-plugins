#include "pj_config_utils/config_utils.hpp"

#include <string>

namespace pj::config {

PJ::Expected<nlohmann::json> parseStrict(std::string_view config_json, std::string_view context) {
  if (config_json.empty()) {
    return nlohmann::json::object();
  }
  auto parsed = nlohmann::json::parse(config_json, nullptr, false);
  if (parsed.is_discarded()) {
    return PJ::unexpected("invalid " + std::string(context) + " JSON");
  }
  return parsed;
}

nlohmann::json parseLenient(std::string_view config_json, bool* out_was_malformed) {
  if (out_was_malformed != nullptr) {
    *out_was_malformed = false;
  }
  if (config_json.empty()) {
    return nlohmann::json::object();
  }
  auto parsed = nlohmann::json::parse(config_json, nullptr, false);
  if (parsed.is_discarded()) {
    if (out_was_malformed != nullptr) {
      *out_was_malformed = true;
    }
    return nlohmann::json::object();
  }
  return parsed;
}

}  // namespace pj::config
