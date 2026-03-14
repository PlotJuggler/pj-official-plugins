#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace {

struct FlattenedField {
  std::string name;
  PJ::sdk::ValueRef value;
};

/// Flatten a JSON value into scalar fields using "/" as separator.
/// Arrays use bracket notation: "arr[0]", "arr[1]", etc.
/// Only numeric and boolean leaves are emitted; strings and nulls are skipped.
void flattenJson(const std::string& prefix, const nlohmann::json& value,
                 std::vector<FlattenedField>& out) {
  switch (value.type()) {
    case nlohmann::detail::value_t::object:
      for (const auto& [key, child] : value.items()) {
        flattenJson(prefix.empty() ? key : prefix + "/" + key, child, out);
      }
      break;

    case nlohmann::detail::value_t::array:
      for (std::size_t i = 0; i < value.size(); ++i) {
        flattenJson(prefix + "[" + std::to_string(i) + "]", value[i], out);
      }
      break;

    case nlohmann::detail::value_t::boolean:
      out.push_back({prefix, value.get<bool>()});
      break;

    case nlohmann::detail::value_t::number_integer:
      out.push_back({prefix, value.get<int64_t>()});
      break;

    case nlohmann::detail::value_t::number_unsigned:
      out.push_back({prefix, value.get<uint64_t>()});
      break;

    case nlohmann::detail::value_t::number_float:
      out.push_back({prefix, value.get<double>()});
      break;

    default:
      break;
  }
}

class JsonParser : public PJ::MessageParserPluginBase {
 public:
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("write host not bound"));
    }

    auto json = nlohmann::json::parse(payload.data(), payload.data() + payload.size(),
                                      /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
      return PJ::unexpected(std::string("invalid JSON payload"));
    }

    if (json.is_array()) {
      for (auto& element : json) {
        auto status = flattenAndAppend(timestamp_ns, element);
        if (!status) return status;
      }
      return PJ::okStatus();
    }

    return flattenAndAppend(timestamp_ns, json);
  }

 private:
  PJ::Status flattenAndAppend(PJ::Timestamp ts, const nlohmann::json& json) {
    owned_fields_.clear();
    flattenJson("", json, owned_fields_);

    // Build string_view references now that owned_fields_ is stable.
    named_fields_.clear();
    named_fields_.reserve(owned_fields_.size());
    for (const auto& f : owned_fields_) {
      named_fields_.push_back({.name = f.name, .value = f.value});
    }

    return writeHost().appendRecord(
        ts, PJ::Span<const PJ::sdk::NamedFieldValue>(named_fields_.data(), named_fields_.size()));
  }

  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::NamedFieldValue> named_fields_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(JsonParser,
                         R"({"name":"JSON Parser","version":"1.0.0","encoding":"json"})")
