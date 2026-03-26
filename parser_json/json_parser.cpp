#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include "json_manifest.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct FlattenedField {
  std::string name;
  PJ::sdk::ValueRef value;
  std::string owned_string;  // keeps string data alive for string_view in value
};

/// Flatten a JSON value into scalar fields using "/" as separator.
/// Arrays use bracket notation: "arr[0]", "arr[1]", etc.
/// Only numeric and boolean leaves are emitted; strings and nulls are skipped.
void flattenJson(const std::string& prefix, const nlohmann::json& value,
                 std::size_t max_array_size, bool clamp_arrays,
                 std::vector<FlattenedField>& out) {
  switch (value.type()) {
    case nlohmann::detail::value_t::object:
      for (const auto& [key, child] : value.items()) {
        flattenJson(prefix.empty() ? key : prefix + "/" + key, child, max_array_size, clamp_arrays, out);
      }
      break;

    case nlohmann::detail::value_t::array: {
      auto count = value.size();
      if (max_array_size > 0 && count > max_array_size) {
        if (!clamp_arrays) break;  // skip oversized
        count = max_array_size;
      }
      for (std::size_t i = 0; i < count; ++i) {
        flattenJson(prefix + "[" + std::to_string(i) + "]", value[i], max_array_size, clamp_arrays, out);
      }
      break;
    }

    case nlohmann::detail::value_t::boolean:
      out.push_back({prefix, value.get<bool>(), {}});
      break;

    case nlohmann::detail::value_t::number_integer:
      out.push_back({prefix, value.get<int64_t>(), {}});
      break;

    case nlohmann::detail::value_t::number_unsigned:
      out.push_back({prefix, value.get<uint64_t>(), {}});
      break;

    case nlohmann::detail::value_t::number_float:
      out.push_back({prefix, value.get<double>(), {}});
      break;

    case nlohmann::detail::value_t::string: {
      auto str = value.get<std::string>();
      if (str.size() < 100) {
        out.push_back({prefix, PJ::sdk::ValueRef{}, std::move(str)});
        out.back().value = std::string_view(out.back().owned_string);
      }
      break;
    }

    default:
      break;
  }
}

class JsonParser : public PJ::MessageParserPluginBase {
 public:
  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded()) {
      encoding_hint_ = cfg.value("encoding_hint", std::string{});
      max_array_size_ = cfg.value("max_array_size", std::size_t{0});
      clamp_large_arrays_ = cfg.value("clamp_large_arrays", true);
    }
    return PJ::okStatus();
  }

  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("write host not bound"));
    }

    // Try parsing in order: JSON → CBOR → MessagePack → BSON
    // (or use encoding_hint_ to skip straight to the right one)
    auto json = tryParse(payload);
    if (json.is_discarded()) {
      return PJ::unexpected(std::string("failed to parse payload as JSON/CBOR/MessagePack/BSON"));
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
    flattenJson("", json, max_array_size_, clamp_large_arrays_, owned_fields_);

    // Fix up string_view entries now that the vector won't reallocate.
    for (auto& f : owned_fields_) {
      if (!f.owned_string.empty()) {
        f.value = std::string_view(f.owned_string);
      }
    }

    bound_fields_.clear();
    bound_fields_.reserve(owned_fields_.size());
    for (const auto& f : owned_fields_) {
      auto it = field_cache_.find(f.name);
      if (it == field_cache_.end()) {
        auto handle = writeHost().ensureField(f.name, PJ::sdk::typeOf(f.value));
        if (!handle.has_value()) {
          return PJ::unexpected(handle.error());
        }
        it = field_cache_.emplace(f.name, *handle).first;
      }
      bound_fields_.push_back({.field = it->second, .value = f.value});
    }

    return writeHost().appendBoundRecord(
        ts, PJ::Span<const PJ::sdk::BoundFieldValue>(bound_fields_.data(), bound_fields_.size()));
  }

  nlohmann::json tryParse(PJ::Span<const uint8_t> payload) {
    const auto* data = payload.data();
    auto size = payload.size();

    if (encoding_hint_ == "cbor") {
      return nlohmann::json::from_cbor(data, data + size, /*strict=*/true, /*allow_exceptions=*/false);
    }
    if (encoding_hint_ == "msgpack") {
      return nlohmann::json::from_msgpack(data, data + size, /*strict=*/true, /*allow_exceptions=*/false);
    }
    if (encoding_hint_ == "bson") {
      return nlohmann::json::from_bson(data, data + size, /*strict=*/true, /*allow_exceptions=*/false);
    }

    // No hint — try JSON first (most common)
    auto result = nlohmann::json::parse(data, data + size, nullptr, false);
    if (!result.is_discarded()) return result;

    // Only try binary formats if the payload starts with a non-ASCII byte
    // (JSON always starts with '{', '[', '"', or a digit — all ASCII)
    if (size > 0 && data[0] > 0x7F) {
      result = nlohmann::json::from_cbor(data, data + size, true, false);
      if (!result.is_discarded()) return result;

      result = nlohmann::json::from_msgpack(data, data + size, true, false);
      if (!result.is_discarded()) return result;

      result = nlohmann::json::from_bson(data, data + size, true, false);
      if (!result.is_discarded()) return result;
    }

    return result;  // still discarded from JSON parse attempt
  }

  std::string encoding_hint_;
  std::size_t max_array_size_ = 0;
  bool clamp_large_arrays_ = true;
  std::unordered_map<std::string, PJ::sdk::FieldHandle> field_cache_;
  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::BoundFieldValue> bound_fields_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(JsonParser, kJsonManifest)
