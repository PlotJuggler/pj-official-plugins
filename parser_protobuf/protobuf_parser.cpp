#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include "protobuf_manifest.hpp"

#include <nlohmann/json.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gp = google::protobuf;

namespace {

struct FlattenedField {
  std::string name;
  PJ::sdk::ValueRef value;
  std::string owned_string;  // keeps string data alive for string_view in value
};

/// Recursively flatten a protobuf message into scalar fields.
/// Nested messages use "/" separator. Repeated fields use "[i]" suffix.
/// Map fields: skip the "key" field, extract the "value" field.
void flattenMessage(const gp::Message& msg, const std::string& prefix, bool is_map,
                    unsigned max_array_size, bool clamp_arrays,
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
      if (max_array_size > 0 && count > max_array_size) {
        if (!clamp_arrays) continue;  // skip oversized arrays
        count = max_array_size;       // clamp to limit
      }
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
          out.push_back({full_key, v, {}});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_FLOAT: {
          float v = repeated ? reflection->GetRepeatedFloat(msg, field, static_cast<int>(idx))
                             : reflection->GetFloat(msg, field);
          out.push_back({full_key, v, {}});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_INT32: {
          int32_t v = repeated ? reflection->GetRepeatedInt32(msg, field, static_cast<int>(idx))
                               : reflection->GetInt32(msg, field);
          out.push_back({full_key, v, {}});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_INT64: {
          int64_t v = repeated ? reflection->GetRepeatedInt64(msg, field, static_cast<int>(idx))
                               : reflection->GetInt64(msg, field);
          out.push_back({full_key, v, {}});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_UINT32: {
          uint32_t v = repeated ? reflection->GetRepeatedUInt32(msg, field, static_cast<int>(idx))
                                : reflection->GetUInt32(msg, field);
          out.push_back({full_key, v, {}});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_UINT64: {
          uint64_t v = repeated ? reflection->GetRepeatedUInt64(msg, field, static_cast<int>(idx))
                                : reflection->GetUInt64(msg, field);
          out.push_back({full_key, v, {}});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_BOOL: {
          bool v = repeated ? reflection->GetRepeatedBool(msg, field, static_cast<int>(idx))
                            : reflection->GetBool(msg, field);
          out.push_back({full_key, v, {}});
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_ENUM: {
          const gp::EnumValueDescriptor* ev =
              repeated ? reflection->GetRepeatedEnum(msg, field, static_cast<int>(idx))
                       : reflection->GetEnum(msg, field);
          // Store enum name as string (matching original PlotJuggler behavior)
          out.push_back({full_key, PJ::sdk::ValueRef{}, std::string(ev->name())});
          out.back().value = std::string_view(out.back().owned_string);
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
            flattenMessage(sub, full_key + map_suffix, false, max_array_size, clamp_arrays, out);
          } else {
            flattenMessage(sub, full_key, false, max_array_size, clamp_arrays, out);
          }
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_STRING: {
          // Include short string fields (< 100 bytes), skip large blobs.
          // This matches original PlotJuggler behavior.
          std::string str_val =
              repeated ? reflection->GetRepeatedString(msg, field, static_cast<int>(idx))
                       : reflection->GetString(msg, field);
          if (str_val.size() < 100) {
            out.push_back({full_key, PJ::sdk::ValueRef{}, std::move(str_val)});
            out.back().value = std::string_view(out.back().owned_string);
          }
          break;
        }
      }
    }
  }
}

/// Map protobuf cpp_type to PJ::PrimitiveType for pre-registration.
PJ::PrimitiveType protobufCppTypeToPrimitive(gp::FieldDescriptor::CppType cpp_type) {
  switch (cpp_type) {
    case gp::FieldDescriptor::CPPTYPE_DOUBLE: return PJ::PrimitiveType::kFloat64;
    case gp::FieldDescriptor::CPPTYPE_FLOAT: return PJ::PrimitiveType::kFloat32;
    case gp::FieldDescriptor::CPPTYPE_INT32: return PJ::PrimitiveType::kInt32;
    case gp::FieldDescriptor::CPPTYPE_INT64: return PJ::PrimitiveType::kInt64;
    case gp::FieldDescriptor::CPPTYPE_UINT32: return PJ::PrimitiveType::kUint32;
    case gp::FieldDescriptor::CPPTYPE_UINT64: return PJ::PrimitiveType::kUint64;
    case gp::FieldDescriptor::CPPTYPE_BOOL: return PJ::PrimitiveType::kBool;
    case gp::FieldDescriptor::CPPTYPE_ENUM: return PJ::PrimitiveType::kString;
    case gp::FieldDescriptor::CPPTYPE_STRING: return PJ::PrimitiveType::kString;
    default: return PJ::PrimitiveType::kUnspecified;
  }
}

/// Walk the descriptor tree and pre-register non-repeated scalar fields.
/// Repeated fields, maps, and nested messages with repeated parents are skipped
/// (they produce dynamic field names like "arr[0]" at runtime).
void preRegisterFields(const gp::Descriptor* descriptor, const std::string& prefix,
                       PJ::sdk::ParserWriteHostView host,
                       std::unordered_map<std::string, PJ::sdk::FieldHandle>& cache) {
  for (int i = 0; i < descriptor->field_count(); ++i) {
    const gp::FieldDescriptor* field = descriptor->field(i);
    if (field->is_repeated() || field->is_map()) {
      continue;
    }

    std::string name(field->name());
    std::string path = prefix.empty() ? name : prefix + "/" + name;

    if (field->cpp_type() == gp::FieldDescriptor::CPPTYPE_MESSAGE) {
      preRegisterFields(field->message_type(), path, host, cache);
      continue;
    }
    auto type = protobufCppTypeToPrimitive(field->cpp_type());
    auto handle = host.ensureField(path, type);
    if (handle.has_value()) {
      cache.emplace(path, *handle);
    }
  }
}

class ProtobufParser : public PJ::MessageParserPluginBase {
 public:
  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded()) {
      use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
      max_array_size_ = cfg.value("max_array_size", 0u);
      clamp_large_arrays_ = cfg.value("clamp_large_arrays", true);
    }
    return PJ::okStatus();
  }

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

    // Detect embedded timestamp field: a top-level non-repeated double named "timestamp"
    timestamp_field_ = nullptr;
    if (use_embedded_timestamp_) {
      for (int i = 0; i < descriptor_->field_count(); ++i) {
        const auto* f = descriptor_->field(i);
        if (f->name() == "timestamp" && !f->is_repeated() &&
            f->cpp_type() == gp::FieldDescriptor::CPPTYPE_DOUBLE) {
          timestamp_field_ = f;
          break;
        }
      }
    }

    if (writeHostBound()) {
      preRegisterFields(descriptor_, "", writeHost(), field_cache_);
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

    // Extract embedded timestamp if available (overrides the provided timestamp)
    if (timestamp_field_ != nullptr) {
      double ts_seconds = msg->GetReflection()->GetDouble(*msg, timestamp_field_);
      timestamp_ns = static_cast<PJ::Timestamp>(ts_seconds * 1e9);
    }

    owned_fields_.clear();
    flattenMessage(*msg, "", false, max_array_size_, clamp_large_arrays_, owned_fields_);

    // Fix up string_view entries now that the vector won't reallocate.
    // During flattenMessage, push_back may have reallocated the vector,
    // invalidating earlier string_view pointers. Re-point them now.
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
        timestamp_ns,
        PJ::Span<const PJ::sdk::BoundFieldValue>(bound_fields_.data(), bound_fields_.size()));
  }

 private:
  std::unique_ptr<gp::DescriptorPool> pool_;
  std::unique_ptr<gp::DynamicMessageFactory> factory_;
  const gp::Descriptor* descriptor_ = nullptr;
  const gp::FieldDescriptor* timestamp_field_ = nullptr;
  bool use_embedded_timestamp_ = false;
  unsigned max_array_size_ = 0;
  bool clamp_large_arrays_ = true;

  std::unordered_map<std::string, PJ::sdk::FieldHandle> field_cache_;
  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::BoundFieldValue> bound_fields_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ProtobufParser, kProtobufManifest)
