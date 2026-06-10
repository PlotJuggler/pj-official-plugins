#include <cstddef>
#include <deque>
#include <nlohmann/json.hpp>
#include <pj_array_policy/array_policy.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <vector>

#include "json_manifest.hpp"
#include "json_parser_dialog.hpp"

namespace {

/// Output target for flattenJson: a fields vector for the decoded
/// NamedFieldValues, and a string buffer that owns the std::string instances
/// the field string_views point into. `strings` is a std::deque so that
/// push_back never invalidates references to earlier elements — short
/// (SSO-range) strings stored before the push would otherwise relocate when
/// a std::vector reallocates, leaving previously-captured string_views
/// dangling.
struct FlattenSink {
  std::vector<PJ::sdk::NamedFieldValue>& fields;
  std::deque<std::string>& strings;
};

/// Flatten a JSON value into NamedFieldValue entries using "/" as separator.
/// Arrays use bracket notation: "arr[0]", "arr[1]", etc.
/// String values shorter than 100 chars are preserved (the std::string is
/// owned by sink.strings); longer strings and nulls are skipped.
void flattenJson(
    const std::string& prefix, const nlohmann::json& value, std::size_t max_array_size, bool clamp_arrays,
    FlattenSink& sink) {
  switch (value.type()) {
    case nlohmann::detail::value_t::object:
      for (const auto& [key, child] : value.items()) {
        flattenJson(prefix.empty() ? key : prefix + "/" + key, child, max_array_size, clamp_arrays, sink);
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
        flattenJson(prefix + "[" + std::to_string(i) + "]", value[i], max_array_size, clamp_arrays, sink);
      }
      break;
    }

    case nlohmann::detail::value_t::boolean:
      sink.fields.push_back({prefix, value.get<bool>()});
      break;

    case nlohmann::detail::value_t::number_integer:
      sink.fields.push_back({prefix, value.get<int64_t>()});
      break;

    case nlohmann::detail::value_t::number_unsigned:
      sink.fields.push_back({prefix, value.get<uint64_t>()});
      break;

    case nlohmann::detail::value_t::number_float:
      sink.fields.push_back({prefix, value.get<double>()});
      break;

    case nlohmann::detail::value_t::string: {
      auto str = value.get<std::string>();
      if (str.size() < 100) {
        sink.strings.push_back(std::move(str));
        sink.fields.push_back({prefix, std::string_view(sink.strings.back())});
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
    // Register under "" so parse() works even before bindSchema() is called
    // (tests and hosts that skip schema binding land on the default empty
    // bound_type_name_ and find this entry).
    registerSchemaHandler("", makeHandler());
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded()) {
      encoding_hint_ = cfg.value("encoding_hint", std::string{});
      array_limit_ = pj::array_policy::arrayLimitFromJson(cfg);
      // Embedded timestamp: when set, doParseScalars reports the field's value
      // as ScalarRecord::ts so the host keys the row by it instead of the
      // transport receive time.
      use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
      timestamp_field_name_ = cfg.value("timestamp_field_name", std::string("timestamp"));
    }
    return PJ::okStatus();
  }

  // Re-register the handler under the actual type name so classifySchema /
  // parseScalars lookups by that name hit it. Idempotent: when the host calls
  // bindSchema repeatedly with the same name, the existing entry is reused
  // instead of allocating a new std::function closure each time.
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
      return status;
    }
    if (findSchemaHandler(type_name) == nullptr) {
      registerSchemaHandler(std::string(type_name), makeHandler());
    }
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

  PJ::Expected<PJ::sdk::ScalarRecord> doParseScalars(PJ::Timestamp /*host_ts*/, PJ::Span<const uint8_t> payload) {
    auto json = tryParse(payload);
    if (json.is_discarded()) {
      return PJ::unexpected(std::string("failed to parse payload as JSON/CBOR/MessagePack/BSON"));
    }

    // string_storage_ owns the std::string instances backing each
    // string_view in rec.fields. Cleared per call; deque keeps element
    // addresses stable across pushes (see FlattenSink doc).
    string_storage_.clear();

    PJ::sdk::ScalarRecord rec;
    // Apply the configured embedded timestamp (nullopt -> host keeps its own).
    rec.ts = extractEmbeddedTimestamp(json);
    FlattenSink sink{rec.fields, string_storage_};
    flattenJson("", json, array_limit_.max_size, array_limit_.clamp(), sink);
    return rec;
  }

  // Returns the embedded timestamp from the JSON object when use_embedded_timestamp_
  // is set and the configured field is present and numeric; otherwise nullopt so
  // the host keeps the message's own timestamp.
  std::optional<PJ::Timestamp> extractEmbeddedTimestamp(const nlohmann::json& json) const {
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
  pj::array_policy::ArrayLimit array_limit_;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_ = "timestamp";
  // Owns the std::string backing every string_view in the returned
  // ScalarRecord. std::deque so push_back keeps addresses of earlier
  // elements stable — see FlattenSink doc above.
  std::deque<std::string> string_storage_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(JsonParser, kJsonManifest)

PJ_DIALOG_PLUGIN(JsonParserDialog)
