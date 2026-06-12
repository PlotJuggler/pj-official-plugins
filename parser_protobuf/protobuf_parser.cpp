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
#include <pj_array_policy/array_policy.hpp>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_base/builtin/video_frame_codec.hpp>
#include <pj_base64/base64.hpp>
#include <pj_laser_scan/laser_scan_projector.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "foxglove_object_codecs.hpp"
#include "foxglove_pointcloud_codec.hpp"
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
    array_limit_ = pj::array_policy::arrayLimitFromJson(cfg);

    // If config contains a compiled schema (from dialog), bind it
    if (cfg.contains("compiled_schema_base64") && cfg["compiled_schema_base64"].is_string() &&
        cfg.contains("message_type") && cfg["message_type"].is_string()) {
      std::string schema_b64 = cfg["compiled_schema_base64"].get<std::string>();
      std::string type_name = cfg["message_type"].get<std::string>();

      if (!schema_b64.empty() && !type_name.empty()) {
        std::string schema_bytes = PJ::base64::decode(schema_b64);
        if (!schema_bytes.empty()) {
          auto status = bindSchema(
              type_name,
              PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema_bytes.data()), schema_bytes.size()));
          if (!status) {
            return status;
          }
        }
      }
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

    // Canonical-object fast path for foxglove.PointCloud. Unlike VideoFrame, the
    // Foxglove point-cloud wire layout differs from the canonical PJ.PointCloud
    // (point_stride vs point_step, PackedElementField with a swapped enum, an
    // extra pose, no width/height), so the SDK codec does not apply — we decode
    // it plugin-local and zero-copy, exactly mirroring how parser_ros promotes
    // sensor_msgs/PointCloud2 to a kPointCloud object. The schema bytes (a real
    // FileDescriptorSet) are intentionally ignored: the layout is known.
    if (type_name == "foxglove.PointCloud") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      registerFoxglovePointCloudHandler(type_name);
      return PJ::okStatus();
    }

    // Canonical-object fast path for foxglove.LaserScan -> kPointCloud. The
    // scan is eagerly projected into cartesian x/y/z(/intensity) points by the
    // shared pj_laser_scan projector (cos/sin LUT cached per scanner config),
    // exactly mirroring how parser_ros promotes sensor_msgs/LaserScan. The
    // schema bytes are intentionally ignored: the layout is known.
    if (type_name == "foxglove.LaserScan") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      registerFoxgloveLaserScanHandler(type_name);
      return PJ::okStatus();
    }

    // Other well-known Foxglove schemas -> their canonical builtin objects.
    // Same rationale as PointCloud: the wire layout is known, so we decode it
    // directly and bypass the descriptor pool. Crucially, promoting these stops
    // the generic scalar-flatten of their large nested arrays (a Foxglove scene
    // / annotation message flattens to a pathological number of scalar series).
    if (type_name == "foxglove.FrameTransform" || type_name == "foxglove.CompressedImage" ||
        type_name == "foxglove.RawImage" || type_name == "foxglove.CameraCalibration" ||
        type_name == "foxglove.ImageAnnotations" || type_name == "foxglove.SceneUpdate") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      registerFoxgloveObjectHandler(type_name);
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
      return PJ::unexpected("failed to deserialize protobuf message (" + std::to_string(payload.size()) + " bytes)");
    }

    // Extract embedded timestamp if available (overrides the provided timestamp)
    if (timestamp_field_ != nullptr) {
      double ts_seconds = msg->GetReflection()->GetDouble(*msg, timestamp_field_);
      timestamp_ns = static_cast<PJ::Timestamp>(ts_seconds * 1e9);
    }

    owned_fields_.clear();
    flattenMessage(*msg, "", false, array_limit_.max_size, array_limit_.clamp(), owned_fields_);

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

  // Register the SchemaHandler for foxglove.PointCloud. The object route decodes
  // one cloud per message zero-copy (the packed-point span aliases the payload);
  // the scalar route emits a slim metadata row (frame_id / point_count /
  // point_step / num_fields) so the ingest path still produces plottable
  // columns, mirroring parseScalarsDiscardingLargeArrays for PointCloud2.
  void registerFoxglovePointCloudHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kPointCloud;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      // No anchor needed: the scalar route reads only metadata and never retains
      // the aliased point buffer past this call.
      auto decoded = pj_protobuf::deserializeFoxglovePointCloudView(payload.data(), payload.size(), nullptr);
      if (!decoded) {
        return PJ::unexpected(std::move(decoded).error());
      }
      const auto& cloud = decoded->cloud;
      pointcloud_frame_id_ = cloud.frame_id;  // keep alive for the string_view below.
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_ && cloud.timestamp_ns > 0) {
        record.ts = cloud.timestamp_ns;
      }
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(pointcloud_frame_id_)}});
      record.fields.push_back({.name = "point_count", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(cloud.width)}});
      record.fields.push_back(
          {.name = "point_step", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(cloud.point_step)}});
      record.fields.push_back(
          {.name = "num_fields", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(cloud.fields.size())}});
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      auto decoded =
          pj_protobuf::deserializeFoxglovePointCloudView(payload.bytes.data(), payload.bytes.size(), payload.anchor);
      if (!decoded) {
        return PJ::unexpected(std::move(decoded).error());
      }
      // sdk::PointCloud expresses geometry in frame_id and relies on an external
      // TF tree (like PointCloud2). Foxglove's inline pose has no home in the
      // struct, so a non-identity pose is dropped — warn once so it is not a
      // silent failure (the cloud would be mislocated if no equivalent TF is
      // present). foxglove.FrameTransform support is the follow-up that fixes it.
      if (decoded->has_pose && !decoded->pose_is_identity && !pointcloud_pose_warned_) {
        pointcloud_pose_warned_ = true;
        std::cerr << "[protobuf_parser] foxglove.PointCloud carries a non-identity pose; it is dropped "
                     "(points kept in frame_id). Provide the matching /tf transform for correct 3D placement.\n";
      }
      std::optional<PJ::Timestamp> ts;
      if (use_embedded_timestamp_ && decoded->cloud.timestamp_ns > 0) {
        ts = decoded->cloud.timestamp_ns;
      }
      return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(decoded->cloud)}};
    };

    registerSchemaHandler(type_name, std::move(handler));
  }

  // Register the SchemaHandler for foxglove.LaserScan. The object route decodes
  // + eagerly projects one scan per message into an OWNED point buffer (no
  // zero-copy by design: the wire carries polar ranges, the cloud carries
  // newly generated cartesian points, anchored by the projector). The scalar
  // route emits a slim metadata row (frame_id / start_angle / end_angle /
  // num_ranges) from a header-only walk — no LUT and no O(N) projection —
  // mirroring the lightweight foxglove.PointCloud scalar path above.
  void registerFoxgloveLaserScanHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kPointCloud;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      auto info = pj_protobuf::readFoxgloveLaserScanInfo(payload.data(), payload.size());
      if (!info) {
        return PJ::unexpected(std::move(info).error());
      }
      laserscan_frame_id_ = std::move(info->frame_id);  // keep alive for the string_view below.
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_ && info->timestamp_ns > 0) {
        record.ts = info->timestamp_ns;
      }
      // Flat metadata names (no '/' prefix), matching the PointCloud handler convention.
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(laserscan_frame_id_)}});
      record.fields.push_back({.name = "start_angle", .value = PJ::sdk::ValueRef{info->start_angle}});
      record.fields.push_back({.name = "end_angle", .value = PJ::sdk::ValueRef{info->end_angle}});
      record.fields.push_back({.name = "num_ranges", .value = PJ::sdk::ValueRef{info->num_ranges}});
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      auto decoded =
          pj_protobuf::deserializeFoxgloveLaserScan(payload.bytes.data(), payload.bytes.size(), laserscan_projector_);
      if (!decoded) {
        return PJ::unexpected(std::move(decoded).error());
      }
      // Same policy as foxglove.PointCloud above: sdk::PointCloud has no pose
      // (geometry lives in frame_id + the TF tree), so a non-identity pose is
      // dropped — warn once so it is not a silent mislocation.
      if (decoded->has_pose && !decoded->pose_is_identity && !laserscan_pose_warned_) {
        laserscan_pose_warned_ = true;
        std::cerr << "[protobuf_parser] foxglove.LaserScan carries a non-identity pose; it is dropped "
                     "(points kept in frame_id). Provide the matching /tf transform for correct 3D placement.\n";
      }
      std::optional<PJ::Timestamp> ts;
      if (use_embedded_timestamp_ && decoded->cloud.timestamp_ns > 0) {
        ts = decoded->cloud.timestamp_ns;
      }
      return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(decoded->cloud)}};
    };

    registerSchemaHandler(type_name, std::move(handler));
  }

  // Register a SchemaHandler for one of the well-known Foxglove scene/image
  // schemas. parse_object decodes the canonical builtin object; parse_scalars
  // emits a slim, BOUNDED metadata row (counts/sizes only) so promoted topics
  // still produce a few plottable columns without the per-element scalar blow-up
  // that made the generic flatten pathological.
  void registerFoxgloveObjectHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    const std::string name(type_name);

    if (name == "foxglove.FrameTransform") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kFrameTransforms;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveFrameTransform(p.bytes.data(), p.bytes.size());
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && !obj->transforms.empty() && obj->transforms.front().timestamp > 0) {
          ts = obj->transforms.front().timestamp;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars = [](PJ::Timestamp, PJ::Span<const uint8_t>) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        PJ::sdk::ScalarRecord r;
        r.fields.push_back({.name = "num_transforms", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(1)}});
        return r;
      };
    } else if (name == "foxglove.CompressedImage") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCompressedImageView(p.bytes.data(), p.bytes.size(), p.anchor);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars = [](PJ::Timestamp,
                                 PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCompressedImageView(payload.data(), payload.size(), nullptr);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());  // surface, don't drop silently
        }
        PJ::sdk::ScalarRecord r;
        r.fields.push_back({.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->data.size())}});
        return r;
      };
    } else if (name == "foxglove.CameraCalibration") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kCameraInfo;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCameraCalibration(p.bytes.data(), p.bytes.size());
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars = [](PJ::Timestamp,
                                 PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCameraCalibration(payload.data(), payload.size());
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());  // surface, don't drop silently
        }
        PJ::sdk::ScalarRecord r;
        r.fields.push_back({.name = "width", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->width)}});
        r.fields.push_back({.name = "height", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->height)}});
        return r;
      };
    } else if (name == "foxglove.ImageAnnotations") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kImageAnnotations;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveImageAnnotations(p.bytes.data(), p.bytes.size());
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp > 0) {
          ts = obj->timestamp;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars = [](PJ::Timestamp,
                                 PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveImageAnnotations(payload.data(), payload.size());
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());  // surface, don't drop silently
        }
        PJ::sdk::ScalarRecord r;
        r.fields.push_back(
            {.name = "num_circles", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->circles.size())}});
        r.fields.push_back(
            {.name = "num_points", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->points.size())}});
        r.fields.push_back({.name = "num_texts", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->texts.size())}});
        return r;
      };
    } else if (name == "foxglove.RawImage") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveRawImageView(p.bytes.data(), p.bytes.size(), p.anchor);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars = [](PJ::Timestamp,
                                 PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveRawImageView(payload.data(), payload.size(), nullptr);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());  // surface, don't drop silently
        }
        PJ::sdk::ScalarRecord r;
        r.fields.push_back({.name = "width", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->width)}});
        r.fields.push_back({.name = "height", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->height)}});
        r.fields.push_back({.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->data.size())}});
        return r;
      };
    } else {  // foxglove.SceneUpdate
      handler.object_type = PJ::sdk::BuiltinObjectType::kSceneEntities;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveSceneUpdate(p.bytes.data(), p.bytes.size());
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && !obj->entities.empty() && obj->entities.front().timestamp > 0) {
          ts = obj->entities.front().timestamp;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars = [](PJ::Timestamp,
                                 PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveSceneUpdate(payload.data(), payload.size());
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());  // surface, don't drop silently
        }
        PJ::sdk::ScalarRecord r;
        r.fields.push_back(
            {.name = "num_entities", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->entities.size())}});
        return r;
      };
    }

    registerSchemaHandler(type_name, std::move(handler));
  }

  std::unique_ptr<gp::DescriptorPool> pool_;
  std::unique_ptr<gp::DynamicMessageFactory> factory_;
  const gp::Descriptor* descriptor_ = nullptr;
  const gp::FieldDescriptor* timestamp_field_ = nullptr;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_;  // empty = fallback chain ("timestamp" → "ts")
  pj::array_policy::ArrayLimit array_limit_;

  std::unordered_map<std::string, PJ::sdk::FieldHandle> field_cache_;
  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::BoundFieldValue> bound_fields_;

  // VideoFrame scalar-route string storage. Keeps the decoded frame_id/format
  // alive while the host reads the ScalarRecord's string_view fields.
  std::string video_frame_id_;
  std::string video_format_;

  // foxglove.PointCloud scalar-route frame_id storage (same lifetime trick) and
  // a latch so the dropped-pose warning fires only once per parser instance.
  std::string pointcloud_frame_id_;
  bool pointcloud_pose_warned_ = false;

  // foxglove.LaserScan: the shared projector (one per parser instance = per
  // topic, so the cos/sin LUT is computed once per scanner config), the
  // scalar-route frame_id storage, and the warn-once dropped-pose latch.
  PJ::laser_scan::LaserScanProjector laserscan_projector_;
  std::string laserscan_frame_id_;
  bool laserscan_pose_warned_ = false;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ProtobufParser, kProtobufManifest)

PJ_DIALOG_PLUGIN(ProtobufParserDialog)
