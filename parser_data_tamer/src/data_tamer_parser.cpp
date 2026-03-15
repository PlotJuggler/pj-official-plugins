#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include <data_tamer_parser/data_tamer_parser.hpp>

#include <string>
#include <vector>

namespace {

class DataTamerParserPlugin : public PJ::MessageParserPluginBase {
 public:
  PJ::Status bindSchema(std::string_view /*type_name*/, PJ::Span<const uint8_t> schema) override {
    std::string schema_text(reinterpret_cast<const char*>(schema.data()), schema.size());
    try {
      schema_ = DataTamerParser::BuilSchemaFromText(schema_text);
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("failed to parse DataTamer schema: ") + e.what());
    }
    return PJ::okStatus();
  }

  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) return PJ::unexpected(std::string("write host not bound"));

    owned_fields_.clear();

    DataTamerParser::SnapshotView snapshot;
    snapshot.schema_hash = schema_.hash;

    DataTamerParser::BufferSpan msg_buffer = {payload.data(), payload.size()};

    auto mask_size = DataTamerParser::Deserialize<uint32_t>(msg_buffer);
    snapshot.active_mask.data = msg_buffer.data;
    snapshot.active_mask.size = mask_size;
    msg_buffer.trimFront(mask_size);

    auto payload_size = DataTamerParser::Deserialize<uint32_t>(msg_buffer);
    snapshot.payload.data = msg_buffer.data;
    snapshot.payload.size = payload_size;

    DataTamerParser::ParseSnapshot(
        schema_, snapshot,
        [this](const std::string& field_name, const DataTamerParser::VarNumber& var) {
          double value = std::visit([](const auto& v) { return static_cast<double>(v); }, var);
          owned_fields_.push_back({"/" + field_name, PJ::sdk::ValueRef{value}});
        });

    if (owned_fields_.empty()) return PJ::okStatus();

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
  DataTamerParser::Schema schema_;

  struct Field {
    std::string name;
    PJ::sdk::ValueRef value;
  };
  std::vector<Field> owned_fields_;
  std::vector<PJ::sdk::NamedFieldValue> named_fields_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(
    DataTamerParserPlugin,
    R"({"name":"DataTamer Parser","version":"1.0.0","encoding":"data_tamer"})")
