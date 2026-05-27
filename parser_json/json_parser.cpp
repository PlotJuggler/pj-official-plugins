#include <cstddef>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <vector>

#include "json_manifest.hpp"
#include "json_parser_dialog.hpp"

namespace {

/// Flatten a JSON value into NamedFieldValue entries using "/" as separator.
/// Arrays use bracket notation: "arr[0]", "arr[1]", etc.
/// String values shorter than 100 chars are preserved; longer strings and
/// nulls are skipped. String storage is appended to `string_storage` so the
/// string_view in the returned ValueRef stays valid for the duration of the
/// caller's appendRecord() call.
void flattenJson(
    const std::string& prefix, const nlohmann::json& value, std::size_t max_array_size, bool clamp_arrays,
    std::vector<PJ::sdk::NamedFieldValue>& out, std::vector<std::string>& string_storage) {
  switch (value.type()) {
    case nlohmann::detail::value_t::object:
      for (const auto& [key, child] : value.items()) {
        flattenJson(
            prefix.empty() ? key : prefix + "/" + key, child, max_array_size, clamp_arrays, out, string_storage);
      }
      break;

    case nlohmann::detail::value_t::array: {
      auto count = value.size();
      if (max_array_size > 0 && count > max_array_size) {
        if (!clamp_arrays) {
          break;
        }
        count = max_array_size;
      }
      for (std::size_t i = 0; i < count; ++i) {
        flattenJson(
            prefix + "[" + std::to_string(i) + "]", value[i], max_array_size, clamp_arrays, out, string_storage);
      }
      break;
    }

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

    case nlohmann::detail::value_t::string: {
      auto str = value.get<std::string>();
      if (str.size() < 100) {
        string_storage.push_back(std::move(str));
        out.push_back({prefix, std::string_view(string_storage.back())});
      }
      break;
    }

    default:
      break;
  }
}

class JsonParser : public PJ::MessageParserPluginBase {
 public:
  JsonParser() {
    // Register under the empty key so parse() works even without a prior
    // bindSchema() call (e.g. in unit tests or hosts that skip schema binding).
    // bindSchema() re-registers under the concrete type name.
    registerSchemaHandler("", makeHandler());
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded()) {
      encoding_hint_ = cfg.value("encoding_hint", std::string{});
      max_array_size_ = cfg.value("max_array_size", std::size_t{0});
      clamp_large_arrays_ = cfg.value("clamp_large_arrays", true);
      // Embedded timestamp: when set, doParseScalars reports the field's value
      // as ScalarRecord::ts so the host keys the row by it instead of the
      // transport receive time.
      use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
      timestamp_field_name_ = cfg.value("timestamp_field_name", std::string("timestamp"));
    }
    return PJ::okStatus();
  }

  // Override bindSchema to register a handler for the specific type_name
  // being bound. The handler table routes parse() → parseScalars() → our
  // lambda, so no parse() override is needed.
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    auto status = MessageParserPluginBase::bindSchema(type_name, schema);
    if (!status) {
      return status;
    }
    registerSchemaHandler(std::string(type_name), makeHandler());
    return PJ::okStatus();
  }

 private:
  PJ::sdk::SchemaHandler makeHandler() {
    return {
        .object_type = PJ::sdk::BuiltinObjectType::kNone,
        .parse_scalars = [this](PJ::Timestamp ts, PJ::Span<const uint8_t> payload)
            -> PJ::Expected<PJ::sdk::ScalarRecord> { return doParseScalars(ts, payload); },
        .parse_object = nullptr};
  }

  PJ::Expected<PJ::sdk::ScalarRecord> doParseScalars(PJ::Timestamp host_ts, PJ::Span<const uint8_t> payload) {
    auto json = tryParse(payload);
    if (json.is_discarded()) {
      return PJ::unexpected(std::string("failed to parse payload as JSON/CBOR/MessagePack/BSON"));
    }

    // string_storage_ keeps string_view values alive across the call chain.
    string_storage_.clear();

    PJ::sdk::ScalarRecord rec;
    // Apply the configured embedded timestamp (nullopt -> host keeps its own).
    rec.ts = extractEmbeddedTimestamp(json, host_ts);
    flattenJson("", json, max_array_size_, clamp_large_arrays_, rec.fields, string_storage_);
    return rec;
  }

  // Returns the embedded timestamp from the JSON object when use_embedded_timestamp_
  // is set and the configured field is present and numeric; otherwise nullopt so
  // the host keeps the message's own timestamp.
  std::optional<PJ::Timestamp> extractEmbeddedTimestamp(const nlohmann::json& json, PJ::Timestamp /*host_ts*/) const {
    if (!use_embedded_timestamp_ || !json.is_object()) {
      return std::nullopt;
    }
    auto it = json.find(timestamp_field_name_);
    if (it == json.end() || !it->is_number()) {
      return std::nullopt;
    }
    const double ts_seconds = it->get<double>();
    return PJ::Timestamp{static_cast<int64_t>(ts_seconds * 1e9)};
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
    if (!result.is_discarded()) {
      return result;
    }

    // Only try binary formats if the payload starts with a non-ASCII byte
    // (JSON always starts with '{', '[', '"', or a digit — all ASCII)
    if (size > 0 && data[0] > 0x7F) {
      result = nlohmann::json::from_cbor(data, data + size, true, false);
      if (!result.is_discarded()) {
        return result;
      }

      result = nlohmann::json::from_msgpack(data, data + size, true, false);
      if (!result.is_discarded()) {
        return result;
      }

      result = nlohmann::json::from_bson(data, data + size, true, false);
      if (!result.is_discarded()) {
        return result;
      }
    }

    return result;  // still discarded from JSON parse attempt
  }

  std::string encoding_hint_;
  std::size_t max_array_size_ = 0;
  bool clamp_large_arrays_ = true;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_ = "timestamp";
  // Storage for string_view values in NamedFieldValue — kept alive across
  // the doParseScalars() → appendRecord() call chain.
  std::vector<std::string> string_storage_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(JsonParser, kJsonManifest)

PJ_DIALOG_PLUGIN(JsonParserDialog)
