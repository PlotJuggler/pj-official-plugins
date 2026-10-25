#include <data_tamer_v2/data_tamer_parser.hpp>
#include <exception>
#include <functional>
#include <optional>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "data_tamer_manifest.hpp"

namespace {

class DataTamerParserPlugin : public PJ::MessageParserPluginBase {
 public:
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    std::string schema_text(reinterpret_cast<const char*>(schema.data()), schema.size());
    try {
      schema_ = DataTamerParser::BuildSchemaFromText(schema_text);
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("failed to parse DataTamer schema: ") + e.what());
    }

    if (auto status = PJ::MessageParserPluginBase::bindSchema(type_name, schema); !status) {
      return status;
    }

    PJ::sdk::SchemaHandler handler;
    handler.parse_scalars = std::bind_front(&DataTamerParserPlugin::parseScalarsImpl, this);
    registerSchemaHandler(std::string(type_name), std::move(handler));
    field_handles_.clear();

    return PJ::okStatus();
  }

  /// Direct ingest appends by field handle. The default parse() goes through
  /// parseScalars, whose records own a copy of every field name and make the
  /// host look each one up again, per field per message.
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("write host not bound"));
    }
    bound_fields_.clear();
    PJ::Status field_status = PJ::okStatus();
    auto status = forEachValue(payload, [&](const std::string& field_name, PJ::sdk::ValueRef value) {
      if (!field_status) {
        return;
      }
      auto it = field_handles_.find(field_name);
      if (it == field_handles_.end()) {
        auto handle = writeHost().ensureField("/" + field_name, PJ::sdk::typeOf(value));
        if (!handle) {
          field_status = PJ::unexpected(handle.error());
          return;
        }
        it = field_handles_.emplace(field_name, *handle).first;
      }
      bound_fields_.push_back({.field = it->second, .value = value});
    });
    if (!status) {
      return status;
    }
    if (!field_status) {
      return field_status;
    }
    if (bound_fields_.empty()) {
      return PJ::okStatus();
    }
    // DataTamer has no payload-embedded timestamp: rows use the receive time.
    return writeHost().appendBoundRecord(
        timestamp_ns, PJ::Span<const PJ::sdk::BoundFieldValue>(bound_fields_.data(), bound_fields_.size()));
  }

 private:
  PJ::Expected<PJ::sdk::ScalarRecord> parseScalarsImpl(
      PJ::Timestamp /*timestamp_ns*/, PJ::Span<const uint8_t> payload) {
    owned_fields_.clear();
    auto status = forEachValue(payload, [this](const std::string& field_name, PJ::sdk::ValueRef value) {
      owned_fields_.push_back({"/" + field_name, value});
    });
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }

    named_fields_.clear();
    named_fields_.reserve(owned_fields_.size());
    for (const auto& f : owned_fields_) {
      named_fields_.push_back({.name = f.name, .value = f.value});
    }
    // ts is nullopt: DataTamer has no payload-embedded timestamp, so the
    // host falls back to the message receive time.
    return PJ::sdk::ScalarRecord{.ts = std::nullopt, .fields = std::move(named_fields_)};
  }

  /// Decodes one snapshot and calls `on_value(field_name, value)` per field in
  /// schema order. `field_name` has no leading '/' and is only valid during the
  /// call.
  template <typename OnValue>
  PJ::Status forEachValue(PJ::Span<const uint8_t> payload, const OnValue& on_value) {
    try {
      auto snapshot = DataTamerParser::SplitMcapMessage({payload.data(), payload.size()});
      snapshot.schema_hash = schema_.hash;

      const bool parsed = DataTamerParser::ParseSnapshot(
          schema_, snapshot, [&on_value](const std::string& field_name, const DataTamerParser::VarNumber& var) {
            auto value = std::visit(
                [](const auto& v) -> PJ::sdk::ValueRef {
                  using T = std::decay_t<decltype(v)>;
                  if constexpr (std::is_same_v<T, char>) {
                    // ValueRef has no char: preserve its wire byte regardless of host char signedness.
                    return static_cast<uint8_t>(v);
                  } else {
                    return v;
                  }
                },
                var);
            on_value(field_name, value);
          });
      if (!parsed) {
        return PJ::unexpected(std::string("DataTamer snapshot payload does not match the schema"));
      }
      return PJ::okStatus();
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("failed to parse DataTamer snapshot: ") + e.what());
    }
  }

  DataTamerParser::Schema schema_;

  struct Field {
    std::string name;
    PJ::sdk::ValueRef value;
  };
  std::vector<Field> owned_fields_;
  std::vector<PJ::sdk::NamedFieldValue> named_fields_;
  std::unordered_map<std::string, PJ::sdk::FieldHandle> field_handles_;  ///< keyed by raw schema name, no '/'
  std::vector<PJ::sdk::BoundFieldValue> bound_fields_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(DataTamerParserPlugin, kDataTamerManifest)
