#include <algorithm>
#include <cstddef>
#include <deque>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "json_manifest.hpp"
#include "json_parser_dialog.hpp"

namespace {

/// Output target for flattenJson: the decoded fields, and the strings their
/// string_views point into. `strings` is a deque so push_back never moves
/// earlier elements; a vector reallocation would relocate SSO strings and
/// dangle the views.
struct FlattenSink {
  std::vector<PJ::sdk::NamedFieldValue>& fields;
  std::deque<std::string>& strings;
};

/// Flattening policy, constant across one parse() call.
struct FlattenOptions {
  PJ::sdk::ArrayLimit limit;
  bool label_keyed_arrays = false;
};

std::string joinPath(const std::string& prefix, const std::string& key) {
  return prefix.empty() ? key : prefix + "/" + key;
}

/// The member ("label", falling back to "name") whose non-empty string value
/// keys an array element; elem.end() when the element is not label-keyed.
nlohmann::json::const_iterator labelMember(const nlohmann::json& elem) {
  if (elem.is_object()) {
    for (const char* key : {"label", "name"}) {
      auto it = elem.find(key);
      if (it != elem.end() && it->is_string() && !it->get_ref<const std::string&>().empty()) {
        return it;
      }
    }
  }
  return elem.end();
}

void flattenJson(const std::string& prefix, const nlohmann::json& value, const FlattenOptions& opts, FlattenSink& sink);

/// Flatten one labeled array element under "<prefix>/<label>", collapsing the
/// canonical {label, value} pair to a single field. Returns false when the
/// element carries no usable label or repeats one already used in this array,
/// so the caller falls back to the indexed name.
bool flattenLabeledElement(
    const std::string& prefix, const nlohmann::json& elem, std::vector<std::string_view>& seen_labels,
    const FlattenOptions& opts, FlattenSink& sink) {
  const auto label_it = labelMember(elem);
  if (label_it == elem.end()) {
    return false;
  }
  const std::string& label = label_it->get_ref<const std::string&>();
  if (std::find(seen_labels.begin(), seen_labels.end(), label) != seen_labels.end()) {
    return false;
  }
  seen_labels.push_back(label);
  const std::string elem_prefix = joinPath(prefix, label);
  if (elem.size() == 2) {
    if (const auto value_it = elem.find("value"); value_it != elem.end()) {
      flattenJson(elem_prefix, *value_it, opts, sink);
      return true;
    }
  }
  for (const auto& [key, child] : elem.items()) {
    if (key != label_it.key()) {
      flattenJson(joinPath(elem_prefix, key), child, opts, sink);
    }
  }
  return true;
}

/// Flattens `value` into sink.fields with "/" separators and "arr[0]" names for
/// array elements (label-keyed arrays: see flattenLabeledElement). Strings of
/// 100 or more chars and nulls are skipped.
void flattenJson(
    const std::string& prefix, const nlohmann::json& value, const FlattenOptions& opts, FlattenSink& sink) {
  switch (value.type()) {
    case nlohmann::detail::value_t::object:
      for (const auto& [key, child] : value.items()) {
        flattenJson(joinPath(prefix, key), child, opts, sink);
      }
      break;

    case nlohmann::detail::value_t::array: {
      auto count = value.size();
      if (opts.limit.max_size > 0 && count > opts.limit.max_size) {
        if (!opts.limit.clamp()) {
          break;
        }
        count = opts.limit.max_size;
      }
      std::vector<std::string_view> seen_labels;  // views into `value`, which outlives the loop
      if (opts.label_keyed_arrays) {
        seen_labels.reserve(count);
      }
      for (std::size_t i = 0; i < count; ++i) {
        if (opts.label_keyed_arrays && flattenLabeledElement(prefix, value[i], seen_labels, opts, sink)) {
          continue;
        }
        flattenJson(prefix + "[" + std::to_string(i) + "]", value[i], opts, sink);
      }
      break;
    }

    case nlohmann::detail::value_t::boolean:
      sink.fields.push_back({prefix, value.get<bool>()});
      break;

    // Every number is a double: JSON (and JavaScript-produced CBOR/MessagePack)
    // writes 1.0 as "1", so a field's integer/float split is not stable across
    // messages, and the host fixes a column's type from its first value.
    // Integers above 2^53 lose precision.
    case nlohmann::detail::value_t::number_integer:
    case nlohmann::detail::value_t::number_unsigned:
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
    // Under "" so parse() works without bindSchema(): an unbound parser looks up the empty type name.
    registerSchemaHandler("", makeHandler());
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded()) {
      encoding_hint_ = cfg.value("encoding_hint", std::string{});
      flatten_opts_.limit = PJ::sdk::arrayLimitFromJson(cfg);
      flatten_opts_.label_keyed_arrays = cfg.value("label_keyed_arrays", false);
      use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
      timestamp_field_name_ = timestampFieldNameOrDefault(cfg.value("timestamp_field_name", std::string{}));
    }
    return PJ::okStatus();
  }

  // Registers the handler under the actual type name; idempotent, so repeated
  // binds of one name do not allocate a new closure each time.
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

    string_storage_.clear();

    PJ::sdk::ScalarRecord rec;
    rec.ts = extractEmbeddedTimestamp(json);
    FlattenSink sink{rec.fields, string_storage_};
    flattenJson("", json, flatten_opts_, sink);
    return rec;
  }

  // nullopt (the host keeps the message's own timestamp) unless use_embedded_timestamp_
  // is set and the configured field is present and numeric. The field is in seconds.
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

    auto result = nlohmann::json::parse(data, data + size, nullptr, false);
    if (!result.is_discarded()) {
      return result;
    }

    // Binary formats only when the first byte is non-ASCII; JSON always starts with an ASCII byte.
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

    return result;
  }

  std::string encoding_hint_;
  FlattenOptions flatten_opts_;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_ = "timestamp";
  // Owns the strings behind the string_views in the returned ScalarRecord (deque: see FlattenSink).
  std::deque<std::string> string_storage_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(JsonParser, kJsonManifest)

PJ_DIALOG_PLUGIN(JsonParserDialog, kJsonManifest)
