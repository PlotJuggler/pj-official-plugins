#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/video_frame_codec.hpp>
#include <pj_base64/base64.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "protobuf_manifest.hpp"
#include "protobuf_parser_dialog.hpp"

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
void flattenMessage(
    const gp::Message& msg, const std::string& prefix, bool is_map, unsigned max_array_size, bool clamp_arrays,
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
        if (!clamp_arrays) {
          continue;  // skip oversized arrays
        }
        count = max_array_size;  // clamp to limit
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
          const gp::EnumValueDescriptor* ev = repeated ? reflection->GetRepeatedEnum(msg, field, static_cast<int>(idx))
                                                       : reflection->GetEnum(msg, field);
          // Store enum name as string (matching original PlotJuggler behavior)
          out.push_back({full_key, PJ::sdk::ValueRef{}, std::string(ev->name())});
          out.back().value = std::string_view(out.back().owned_string);
          break;
        }
        case gp::FieldDescriptor::CPPTYPE_MESSAGE: {
#pragma push_macro("GetMessage")
#undef GetMessage
          const gp::Message& sub = repeated ? reflection->GetRepeatedMessage(msg, field, static_cast<int>(idx))
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
          std::string str_val = repeated ? reflection->GetRepeatedString(msg, field, static_cast<int>(idx))
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
    case gp::FieldDescriptor::CPPTYPE_DOUBLE:
      return PJ::PrimitiveType::kFloat64;
    case gp::FieldDescriptor::CPPTYPE_FLOAT:
      return PJ::PrimitiveType::kFloat32;
    case gp::FieldDescriptor::CPPTYPE_INT32:
      return PJ::PrimitiveType::kInt32;
    case gp::FieldDescriptor::CPPTYPE_INT64:
      return PJ::PrimitiveType::kInt64;
    case gp::FieldDescriptor::CPPTYPE_UINT32:
      return PJ::PrimitiveType::kUint32;
    case gp::FieldDescriptor::CPPTYPE_UINT64:
      return PJ::PrimitiveType::kUint64;
    case gp::FieldDescriptor::CPPTYPE_BOOL:
      return PJ::PrimitiveType::kBool;
    case gp::FieldDescriptor::CPPTYPE_ENUM:
      return PJ::PrimitiveType::kString;
    case gp::FieldDescriptor::CPPTYPE_STRING:
      return PJ::PrimitiveType::kString;
    default:
      return PJ::PrimitiveType::kUnspecified;
  }
}

/// Walk the descriptor tree and pre-register non-repeated scalar fields.
/// Repeated fields, maps, and nested messages with repeated parents are skipped
/// (they produce dynamic field names like "arr[0]" at runtime).
void preRegisterFields(
    const gp::Descriptor* descriptor, const std::string& prefix, PJ::sdk::ParserWriteHostView host,
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
    if (cfg.is_discarded()) {
      return PJ::okStatus();
    }

    // Load options
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    timestamp_field_name_ = cfg.value("timestamp_field_name", std::string{});
    max_array_size_ = cfg.value("max_array_size", 0u);
    clamp_large_arrays_ = cfg.value("clamp_large_arrays", true);

    // If config contains a compiled schema (from dialog), bind it
    if (cfg.contains("compiled_schema_base64") && cfg["compiled_schema_base64"].is_string() &&
        cfg.contains("message_type") && cfg["message_type"].is_string()) {
      std::string schema_b64 = cfg["compiled_schema_base64"].get<std::string>();
      std::string type_name = cfg["message_type"].get<std::string>();

      std::cerr << "[protobuf_parser] loadConfig: message_type='" << type_name
                << "', schema_b64_len=" << schema_b64.size() << "\n";

      if (!schema_b64.empty() && !type_name.empty()) {
        std::string schema_bytes = PJ::base64::decode(schema_b64);
        std::cerr << "[protobuf_parser] decoded schema_bytes_len=" << schema_bytes.size() << "\n";
        if (!schema_bytes.empty()) {
          auto status = bindSchema(
              type_name,
              PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema_bytes.data()), schema_bytes.size()));
          if (!status) {
            std::cerr << "[protobuf_parser] bindSchema failed: " << status.error() << "\n";
            return status;
          }
          std::cerr << "[protobuf_parser] bindSchema succeeded for '" << type_name << "'\n";
        }
      }
    } else {
      std::cerr << "[protobuf_parser] loadConfig: no compiled_schema_base64 or message_type in config\n";
    }

    return PJ::okStatus();
  }

  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    // Canonical-object fast path. PJ.VideoFrame and foxglove.CompressedVideo are
    // wire-identical (a fixed protobuf layout — see the SDK contract), so a
    // single decoder serves both. We bypass the reflection/descriptor pool
    // entirely: the wire layout is known, and deserializeVideoFrameView() reads
    // it zero-copy (the H.264/H.265/… bitstream span aliases the payload). The
    // descriptor-pool machinery below is only needed for arbitrary user schemas.
    if (type_name == PJ::kSchemaVideoFrame || type_name == "foxglove.CompressedVideo") {
      // Record the bound type so the base class dispatch finds our handler.
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      registerVideoFrameHandler(type_name);
      return PJ::okStatus();
    }

    // When schema bytes are empty (e.g. UDP/stream sources that have no schema),
    // the schema was already bound via loadConfig()'s compiled_schema_base64 path.
    // Just record the type name and keep the existing descriptor.
    if (schema.empty()) {
      MessageParserPluginBase::bindSchema(type_name, schema);
      return PJ::okStatus();
    }

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

    // Detect embedded timestamp field: a top-level non-repeated double.
    // Resolution order (same contract as parser_json):
    //   1. If timestamp_field_name_ is set, only that name is tried.
    //   2. If empty, try "timestamp" then "ts" (conventional defaults).
    timestamp_field_ = nullptr;
    if (use_embedded_timestamp_) {
      auto tryField = [&](const std::string& name) -> const gp::FieldDescriptor* {
        for (int i = 0; i < descriptor_->field_count(); ++i) {
          const auto* f = descriptor_->field(i);
          if (f->name() == name && !f->is_repeated() && f->cpp_type() == gp::FieldDescriptor::CPPTYPE_DOUBLE) {
            return f;
          }
        }
        return nullptr;
      };

      if (!timestamp_field_name_.empty()) {
        timestamp_field_ = tryField(timestamp_field_name_);
      } else {
        static const std::array<std::string, 2> kDefaults = {"timestamp", "ts"};
        for (const auto& name : kDefaults) {
          timestamp_field_ = tryField(name);
          if (timestamp_field_) {
            break;
          }
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
    // Canonical-object schemas (VideoFrame) have no descriptor pool — they are
    // served by a registered SchemaHandler. Defer to the base parse(), which
    // dispatches through parseScalars() to that handler.
    if (descriptor_ == nullptr) {
      if (findSchemaHandler(bound_type_name_) != nullptr) {
        return MessageParserPluginBase::parse(timestamp_ns, payload);
      }
      return PJ::unexpected(std::string("no schema bound"));
    }

    const gp::Message* prototype = factory_->GetPrototype(descriptor_);
    std::unique_ptr<gp::Message> msg(prototype->New());
    if (!msg->ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
      std::cerr << "[protobuf_parser] parse failed: payload_size=" << payload.size() << ", first_bytes=";
      for (size_t i = 0; i < std::min<size_t>(16, payload.size()); i++) {
        std::cerr << std::hex << static_cast<int>(payload[i]) << " ";
      }
      std::cerr << std::dec << "\n";
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
        timestamp_ns, PJ::Span<const PJ::sdk::BoundFieldValue>(bound_fields_.data(), bound_fields_.size()));
  }

 private:
  // Register the SchemaHandler for the canonical VideoFrame fast path. The
  // object route decodes one PJ.VideoFrame / foxglove.CompressedVideo per
  // message zero-copy; the scalar route emits a slim metadata row so the
  // ingest path still produces plottable columns (frame_id / format / data
  // size) alongside the object, mirroring the Image/PointCloud handlers in
  // parser_ros.
  void registerVideoFrameHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kVideoFrame;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      // Owning decode (not the zero-copy view): the scalar route only needs the
      // metadata strings + data size, and an owning frame avoids leaving a span
      // that aliases `payload` with a null anchor (a use-after-free footgun if
      // this handler is ever extended to surface the bytes).
      auto frame = PJ::deserializeVideoFrame(payload.data(), payload.size());
      if (!frame) {
        return PJ::unexpected(std::move(frame).error());
      }
      // ValueRef holds a non-owning string_view, so the decoded strings must
      // outlive the returned ScalarRecord (the host reads its fields after this
      // lambda returns). Park them in member storage — same lifetime trick the
      // generic path uses with owned_fields_.
      video_frame_id_ = std::move(frame->frame_id);
      video_format_ = std::move(frame->format);
      PJ::sdk::ScalarRecord record;
      // Match the object route's timestamp policy so the scalar columns and the
      // object entry land on the same timeline (otherwise scalars use host time
      // while the object uses the embedded sensor time).
      if (use_embedded_timestamp_ && frame->timestamp_ns > 0) {
        record.ts = frame->timestamp_ns;
      }
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(video_frame_id_)}});
      record.fields.push_back({.name = "format", .value = PJ::sdk::ValueRef{std::string_view(video_format_)}});
      record.fields.push_back(
          {.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(frame->data.size())}});
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      // Zero-copy: the decoded frame's data span aliases payload.bytes; the
      // anchor keeps that buffer alive past this call.
      auto frame = PJ::deserializeVideoFrameView(payload.bytes.data(), payload.bytes.size(), payload.anchor);
      if (!frame) {
        return PJ::unexpected(std::move(frame).error());
      }
      // Embedded-timestamp policy mirrors the generic path: when enabled, the
      // proto Timestamp drives the record time; otherwise the host's receive
      // time is used.
      std::optional<PJ::Timestamp> ts;
      if (use_embedded_timestamp_ && frame->timestamp_ns > 0) {
        ts = frame->timestamp_ns;
      }
      return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*frame)}};
    };

    registerSchemaHandler(type_name, std::move(handler));
  }

  std::unique_ptr<gp::DescriptorPool> pool_;
  std::unique_ptr<gp::DynamicMessageFactory> factory_;
  const gp::Descriptor* descriptor_ = nullptr;
  const gp::FieldDescriptor* timestamp_field_ = nullptr;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_;  // empty = fallback chain ("timestamp" → "ts")
  unsigned max_array_size_ = 0;
  bool clamp_large_arrays_ = true;

  std::unordered_map<std::string, PJ::sdk::FieldHandle> field_cache_;
  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::BoundFieldValue> bound_fields_;

  // VideoFrame scalar-route string storage. Keeps the decoded frame_id/format
  // alive while the host reads the ScalarRecord's string_view fields.
  std::string video_frame_id_;
  std::string video_format_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ProtobufParser, kProtobufManifest)

PJ_DIALOG_PLUGIN(ProtobufParserDialog)
