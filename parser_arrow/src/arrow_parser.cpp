#include <cstdint>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "arrow_manifest.hpp"

namespace {

/// User-configurable shaping and timestamp options for Arrow IPC streams.
struct ArrowParserConfig {
  std::string timestamp_column;
  bool flatten_structs = true;
  int64_t synthetic_interval_ns = 0;
};

/// Message parser scaffold for self-describing Arrow IPC streams.
class ArrowParser : public PJ::MessageParserPluginBase {
 public:
  /// Bind a self-describing IPC stream type and register its non-canonical schema classification.
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema_bytes) override {
    if (auto status = MessageParserPluginBase::bindSchema(type_name, schema_bytes); !status) {
      return status;
    }
    registerSchemaHandler(
        type_name, PJ::sdk::SchemaHandler{
                       .object_type = PJ::sdk::BuiltinObjectType::kNone,
                       .parse_scalars = nullptr,
                       .parse_object = nullptr,
                   });
    return PJ::okStatus();
  }

  /// Load parser options from the JSON configuration contract.
  PJ::Status loadConfig(std::string_view config_json) override {
    const auto parsed = nlohmann::json::parse(config_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return PJ::unexpected("parser_arrow: malformed configuration JSON");
    }

    try {
      ArrowParserConfig loaded;
      loaded.timestamp_column = parsed.value("timestamp_column", std::string{});
      loaded.flatten_structs = parsed.value("flatten_structs", true);
      loaded.synthetic_interval_ns = parsed.value("synthetic_interval_ns", int64_t{0});
      config_ = std::move(loaded);
    } catch (const nlohmann::json::exception& error) {
      return PJ::unexpected(std::string("parser_arrow: invalid configuration: ") + error.what());
    }
    return PJ::okStatus();
  }

  /// Serialize all parser options to the JSON configuration contract.
  [[nodiscard]] std::string saveConfig() const override {
    return nlohmann::json{
        {"timestamp_column", config_.timestamp_column},
        {"flatten_structs", config_.flatten_structs},
        {"synthetic_interval_ns", config_.synthetic_interval_ns},
    }
        .dump();
  }

  /// Reject payload parsing until the Arrow IPC decoder is implemented.
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    (void)timestamp_ns;
    (void)payload;
    return PJ::unexpected("parser_arrow: not implemented");
  }

 private:
  ArrowParserConfig config_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ArrowParser, kArrowManifest)
