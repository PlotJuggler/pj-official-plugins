#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include <nlohmann/json.hpp>
#include <rosx_introspection/ros_parser.hpp>

#include <string>
#include <vector>

namespace {

/// Owned field: name string + value variant for writeHost().
struct FlattenedField {
  std::string name;
  PJ::sdk::ValueRef value;
};

/// Map a rosx_introspection BuiltinType to a PJ::sdk::ValueRef.
/// Returns the value converted to the appropriate C++ type so that
/// the host receives full-precision integers (not just double).
PJ::sdk::ValueRef variantToValueRef(const RosMsgParser::Variant& variant) {
  using BT = RosMsgParser::BuiltinType;
  switch (variant.getTypeID()) {
    case BT::BOOL:
      return variant.convert<double>() != 0.0;
    case BT::CHAR:
    case BT::INT8:
      return variant.extract<int8_t>();
    case BT::UINT8:
    case BT::BYTE:
      return variant.extract<uint8_t>();
    case BT::INT16:
      return variant.extract<int16_t>();
    case BT::UINT16:
      return variant.extract<uint16_t>();
    case BT::INT32:
      return variant.extract<int32_t>();
    case BT::UINT32:
      return variant.extract<uint32_t>();
    case BT::INT64:
      return variant.extract<int64_t>();
    case BT::UINT64:
      return variant.extract<uint64_t>();
    case BT::FLOAT32:
      return variant.extract<float>();
    case BT::FLOAT64:
      return variant.extract<double>();
    case BT::TIME:
    case BT::DURATION:
      return variant.convert<double>();
    case BT::STRING:
      // Handled separately in parse(); should not reach here.
      return PJ::NullValue{};
    default:
      return variant.convert<double>();
  }
}

class RosParser : public PJ::MessageParserPluginBase {
 public:
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    // schema bytes are the ROS message definition text.
    std::string definition(reinterpret_cast<const char*>(schema.data()), schema.size());

    // Normalise ROS 2 type names: "pkg/msg/Type" -> "pkg/Type"
    type_name_ = std::string(type_name);
    std::string msg_type = type_name_;
    if (auto pos = msg_type.find("/msg/"); pos != std::string::npos) {
      msg_type.erase(pos, 4);  // remove "/msg" (keep the second "/")
    }

    try {
      parser_.emplace("", RosMsgParser::ROSType(msg_type), definition);
      parser_->setMaxArrayPolicy(RosMsgParser::Parser::KEEP_LARGE_ARRAYS, max_array_size_);
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("failed to parse ROS schema: ") + e.what());
    }
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["max_array_size"] = max_array_size_;
    return cfg.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::okStatus();  // use defaults
    }
    max_array_size_ = static_cast<size_t>(cfg.value("max_array_size", 500));
    // If parser already exists, update its policy.
    if (parser_.has_value()) {
      parser_->setMaxArrayPolicy(RosMsgParser::Parser::KEEP_LARGE_ARRAYS, max_array_size_);
    }
    return PJ::okStatus();
  }

  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("write host not bound"));
    }
    if (!parser_.has_value()) {
      return PJ::unexpected(std::string("no schema bound"));
    }

    try {
      parser_->deserialize(
          RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()),
          &flat_msg_, &deserializer_);
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("CDR deserialization failed: ") + e.what());
    }

    owned_fields_.clear();
    string_storage_.clear();
    std::string field_name;

    // All fields (numeric, boolean, and string) are in flat_msg_.value.
    for (const auto& [key, variant] : flat_msg_.value) {
      key.toStr(field_name);

      if (variant.getTypeID() == RosMsgParser::STRING) {
        // String fields: extract into owned storage so string_view remains valid.
        string_storage_.push_back(variant.extract<std::string>());
        owned_fields_.push_back(
            {field_name, PJ::sdk::ValueRef{std::string_view(string_storage_.back())}});
      } else {
        owned_fields_.push_back({field_name, variantToValueRef(variant)});
      }
    }

    // Build the NamedFieldValue span (string_view references into owned_fields_).
    named_fields_.clear();
    named_fields_.reserve(owned_fields_.size());
    for (const auto& f : owned_fields_) {
      named_fields_.push_back({.name = f.name, .value = f.value});
    }

    return writeHost().appendRecord(
        timestamp_ns,
        PJ::Span<const PJ::sdk::NamedFieldValue>(named_fields_.data(), named_fields_.size()));
  }

 private:
  std::string type_name_;
  size_t max_array_size_ = 500;
  std::optional<RosMsgParser::Parser> parser_;
  RosMsgParser::NanoCDR_Deserializer deserializer_;
  RosMsgParser::FlatMessage flat_msg_;

  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::NamedFieldValue> named_fields_;
  std::vector<std::string> string_storage_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(RosParser,
                         R"({"name":"ROS CDR Parser","version":"1.0.0","encoding":"cdr"})")
