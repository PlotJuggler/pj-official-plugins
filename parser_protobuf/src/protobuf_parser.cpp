#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gp = google::protobuf;

namespace {

struct FlattenedField {
  std::string name;
  PJ::sdk::ValueRef value;
};

/// Recursively flatten a protobuf message into scalar fields.
/// Nested messages use "/" separator. Repeated fields use "[i]" suffix.
/// Map fields: skip the "key" field, extract the "value" field.
/// String and bytes fields are skipped.
void flattenMessage(const gp::Message& msg, const std::string& prefix, bool is_map,
                    std::vector<FlattenedField>& out) {
  const gp::Reflection* reflection = msg.GetReflection();
  const gp::Descriptor* descriptor = msg.GetDescriptor();

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const gp::FieldDescriptor* field = descriptor->field(i);

    std::string field_name(field->name());
    std::string key;
    if (is_map) {
      // Map entries have "key" and "value" fields.
      // Skip the key field; use the parent prefix for value.
      if (field_name == "key") {
        continue;
      }
      key = prefix;
    } else {
      key = prefix.empty() ? field_name : prefix + "/" + field_name;
    }

    unsigned count = 1;
    bool repeated = field->is_repeated();
    if (repeated) {
      count = static_cast<unsigned>(reflection->FieldSize(msg, field));
    }

    for (unsigned idx = 0; idx < count; ++idx) {
      std::string full_key = key;
      if (repeated) {
        full_key += "[" + std::to_string(idx) + "]";
      }

      switch (field->cpp_type()) {
        case gp::FieldDescriptor::CPPTYPE_DOUBLE: {
          double v = repeated ? reflection->GetRepeatedDouble(msg, field, static_cast<int>(idx))
                              : reflection->GetDouble(msg, field);
          out.push_back({full_key, v});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_FLOAT: {
          float v = repeated ? reflection->GetRepeatedFloat(msg, field, static_cast<int>(idx))
                             : reflection->GetFloat(msg, field);
          out.push_back({full_key, v});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_INT32: {
          int32_t v = repeated ? reflection->GetRepeatedInt32(msg, field, static_cast<int>(idx))
                               : reflection->GetInt32(msg, field);
          out.push_back({full_key, v});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_INT64: {
          int64_t v = repeated ? reflection->GetRepeatedInt64(msg, field, static_cast<int>(idx))
                               : reflection->GetInt64(msg, field);
          out.push_back({full_key, v});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_UINT32: {
          uint32_t v = repeated ? reflection->GetRepeatedUInt32(msg, field, static_cast<int>(idx))
                                : reflection->GetUInt32(msg, field);
          out.push_back({full_key, v});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_UINT64: {
          uint64_t v = repeated ? reflection->GetRepeatedUInt64(msg, field, static_cast<int>(idx))
                                : reflection->GetUInt64(msg, field);
          out.push_back({full_key, v});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_BOOL: {
          bool v = repeated ? reflection->GetRepeatedBool(msg, field, static_cast<int>(idx))
                            : reflection->GetBool(msg, field);
          out.push_back({full_key, v});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_ENUM: {
          const gp::EnumValueDescriptor* ev =
              repeated ? reflection->GetRepeatedEnum(msg, field, static_cast<int>(idx))
                       : reflection->GetEnum(msg, field);
          out.push_back({full_key, static_cast<int32_t>(ev->number())});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_MESSAGE: {
#pragma push_macro("GetMessage")
#undef GetMessage
          const gp::Message& sub = repeated
                                       ? reflection->GetRepeatedMessage(msg, field, static_cast<int>(idx))
                                       : reflection->GetMessage(msg, field);
#pragma pop_macro("GetMessage")

          if (field->is_map()) {
            // Extract the map key to build a meaningful suffix.
            const gp::Descriptor* map_desc = sub.GetDescriptor();
            const gp::Reflection* map_ref = sub.GetReflection();
            const gp::FieldDescriptor* key_field = map_desc->FindFieldByName("key");
            std::string map_suffix;
            if (key_field != nullptr) {
              switch (key_field->cpp_type()) {
                case gp::FieldDescriptor::CPPTYPE_STRING:
                  map_suffix = "/" + map_ref->GetString(sub, key_field);
                  break;
                case gp::FieldDescriptor::CPPTYPE_INT32:
                  map_suffix = "/" + std::to_string(map_ref->GetInt32(sub, key_field));
                  break;
                case gp::FieldDescriptor::CPPTYPE_INT64:
                  map_suffix = "/" + std::to_string(map_ref->GetInt64(sub, key_field));
                  break;
                case gp::FieldDescriptor::CPPTYPE_UINT32:
                  map_suffix = "/" + std::to_string(map_ref->GetUInt32(sub, key_field));
                  break;
                case gp::FieldDescriptor::CPPTYPE_UINT64:
                  map_suffix = "/" + std::to_string(map_ref->GetUInt64(sub, key_field));
                  break;
                default:
                  break;
              }
            }
            flattenMessage(sub, full_key + map_suffix, true, out);
          } else {
            flattenMessage(sub, full_key, false, out);
          }
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_STRING:
          // Skip string and bytes fields.
          break;
      }
    }
  }
}

class ProtobufParser : public PJ::MessageParserPluginBase {
 public:
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    gp::FileDescriptorSet fd_set;
    if (!fd_set.ParseFromArray(schema.data(), static_cast<int>(schema.size()))) {
      return PJ::unexpected(std::string("failed to parse FileDescriptorSet"));
    }

    pool_ = std::make_unique<gp::DescriptorPool>();
    factory_ = std::make_unique<gp::DynamicMessageFactory>(pool_.get());

    for (int i = 0; i < fd_set.file_size(); ++i) {
      const auto& file = fd_set.file(i);
      pool_->BuildFile(file);
    }

    descriptor_ = pool_->FindMessageTypeByName(std::string(type_name));
    if (descriptor_ == nullptr) {
      return PJ::unexpected(std::string("message type not found: ") + std::string(type_name));
    }

    return PJ::okStatus();
  }

  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("write host not bound"));
    }
    if (descriptor_ == nullptr) {
      return PJ::unexpected(std::string("no schema bound"));
    }

    const gp::Message* prototype = factory_->GetPrototype(descriptor_);
    std::unique_ptr<gp::Message> msg(prototype->New());
    if (!msg->ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
      return PJ::unexpected(std::string("failed to deserialize protobuf message"));
    }

    owned_fields_.clear();
    flattenMessage(*msg, "", false, owned_fields_);

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
  std::unique_ptr<gp::DescriptorPool> pool_;
  std::unique_ptr<gp::DynamicMessageFactory> factory_;
  const gp::Descriptor* descriptor_ = nullptr;

  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::NamedFieldValue> named_fields_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ProtobufParser,
                         R"({"name":"Protobuf Parser","version":"1.0.0","encoding":"protobuf"})")
