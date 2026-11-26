#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/log_codec.hpp>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_base/builtin/poses_in_frame_codec.hpp>
#include <pj_base/builtin/video_frame_codec.hpp>
#include <pj_base64/base64.hpp>
#include <pj_laser_scan/laser_scan_projector.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "foxglove_grid_codec.hpp"
#include "foxglove_object_codecs.hpp"
#include "foxglove_pointcloud_codec.hpp"
#include "foxglove_voxelgrid_codec.hpp"
#include "protobuf_manifest.hpp"
#include "protobuf_parser_dialog.hpp"

namespace gp = google::protobuf;

namespace {

/// Adds every file of a schema to `database`. A pool built on the database resolves imports on
/// demand, so the set may list a file before the files it imports, as the MCAP C++ example does.
void addSchemaFiles(const gp::FileDescriptorSet& fd_set, gp::SimpleDescriptorDatabase& database) {
  for (const auto& file : fd_set.file()) {
    database.Add(file);
  }
}

struct FlattenedField {
  std::string name;
  PJ::sdk::ValueRef value;
  std::string owned_string;  // keeps string data alive for string_view in value
};

/// Seconds form of an absolute nanosecond stamp. Every promoted (object-bearing) schema emits its embedded sensor stamp
/// as a "timestamp" scalar series in these units, unconditionally: use_embedded_timestamp only governs which clock the
/// ROW lands on (ScalarRecord::ts), never whether the series exists.
constexpr double timestampSeconds(PJ::Timestamp ts_ns) {
  return static_cast<double>(ts_ns) * 1e-9;
}

/// Recursively flatten a protobuf message into scalar fields.
/// Nested messages use "/" separator. Repeated fields use "[i]" suffix.
/// Map fields: skip the "key" field, extract the "value" field.
/// A field with explicit presence that the sender omitted (proto2 fields, proto3
/// `optional`, inactive oneof members, unset sub-messages) produces nothing, so the
/// store records null instead of the type's default. Implicit-presence proto3
/// scalars cannot tell "omitted" from the default and still read as 0. A map
/// entry is exempt: its omitted key or value still means the default.
void flattenMessage(
    const gp::Message& msg, const std::string& prefix, bool is_map, unsigned max_array_size, bool clamp_arrays,
    std::vector<FlattenedField>& out) {
  const gp::Reflection* reflection = msg.GetReflection();
  const gp::Descriptor* descriptor = msg.GetDescriptor();
  const bool is_map_entry = descriptor->options().map_entry();

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const gp::FieldDescriptor* field = descriptor->field(i);
    if (!is_map_entry && field->has_presence() && !reflection->HasField(msg, field)) {
      continue;
    }

    std::string field_name(field->name());
    std::string key;
    if (is_map) {
      // Map entries: skip the key field; the value uses the parent prefix.
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
          continue;
        }
        count = max_array_size;
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
          // Only short strings (< 100 bytes) are kept, skipping large blobs, like the original PlotJuggler.
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

/// Repeated fields, maps, and nested messages with repeated parents are skipped: they produce dynamic field names like
/// "arr[0]" at runtime.
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

    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    timestamp_field_name_ = cfg.value("timestamp_field_name", std::string{});
    array_limit_ = PJ::sdk::arrayLimitFromJson(cfg);

    // A compiled schema from the dialog is bound here.
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
    // Canonical-object fast path. PJ.VideoFrame and foxglove.CompressedVideo are wire-identical, so one decoder serves
    // both. The descriptor pool is bypassed: deserializeVideoFrameView() reads the known layout zero-copy (the
    // bitstream span aliases the payload). The pool below is only needed for arbitrary user schemas.
    if (type_name == PJ::kSchemaVideoFrame || type_name == "foxglove.CompressedVideo") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      registerVideoFrameHandler(type_name);
      return PJ::okStatus();
    }

    // foxglove.PointCloud: unlike VideoFrame, its wire layout differs from the canonical PJ.PointCloud (point_stride vs
    // point_step, PackedElementField with a swapped enum, an extra pose, no width/height), so the SDK codec does not
    // apply. It is decoded plugin-local and zero-copy, like parser_ros does for sensor_msgs/PointCloud2. The schema
    // bytes only feed resolveFoxgloveFieldNumbers(); the layout is otherwise known.
    if (type_name == "foxglove.PointCloud") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      resolveFoxgloveFieldNumbers(type_name, schema);
      registerFoxglovePointCloudHandler(type_name);
      return PJ::okStatus();
    }

    // foxglove.LaserScan -> kPointCloud: eagerly projected into cartesian x/y/z(/intensity) points by the shared
    // pj_laser_scan projector (cos/sin LUT cached per scanner config), like parser_ros for sensor_msgs/LaserScan.
    if (type_name == "foxglove.LaserScan") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      resolveFoxgloveFieldNumbers(type_name, schema);
      registerFoxgloveLaserScanHandler(type_name);
      return PJ::okStatus();
    }

    // PJ.PosesInFrame and foxglove.PosesInFrame are wire-identical, so the SDK codec (deserializePosesInFrame) serves
    // both. foxglove.PoseInFrame (a SINGLE pose at field 3) is wire-identical to a one-element PosesInFrame, so the
    // same codec decodes it too.
    if (type_name == PJ::kSchemaPosesInFrame || type_name == "foxglove.PosesInFrame" ||
        type_name == "foxglove.PoseInFrame") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      registerPosesInFrameHandler(type_name);
      return PJ::okStatus();
    }

    // foxglove.Odometry -> kPosesInFrame (one pose). The pose is at field 4 (field 3 is a string), so unlike
    // PoseInFrame the PosesInFrame codec cannot be reused; a dedicated decoder reads the pose and skips velocities,
    // covariances and metadata.
    if (type_name == "foxglove.Odometry") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      resolveFoxgloveFieldNumbers(type_name, schema);
      registerFoxgloveOdometryHandler(type_name);
      return PJ::okStatus();
    }

    // Other well-known Foxglove schemas -> their canonical builtin objects, decoded directly from the known wire
    // layout. Promoting them also avoids the generic scalar flatten of their large nested arrays, which would create a
    // pathological number of series for a scene or annotation message.
    if (type_name == "foxglove.FrameTransform" || type_name == "foxglove.CompressedImage" ||
        type_name == "foxglove.RawImage" || type_name == "foxglove.CameraCalibration" ||
        type_name == "foxglove.ImageAnnotations" || type_name == "foxglove.SceneUpdate" ||
        type_name == "foxglove.VoxelGrid" || type_name == "foxglove.Grid") {
      if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
        return status;
      }
      resolveFoxgloveFieldNumbers(type_name, schema);
      registerFoxgloveObjectHandler(type_name);
      return PJ::okStatus();
    }

    // Log messages keep the generic descriptor-pool path below for their scalar route and add a kLog object route
    // (registerLogHandler).
    const bool is_log = type_name == PJ::kSchemaLog || type_name == "foxglove.Log";

    // Empty schema bytes (e.g. UDP/stream sources) mean the schema was already bound via loadConfig()'s
    // compiled_schema_base64; only the type name is recorded and the existing descriptor is kept.
    if (schema.empty()) {
      MessageParserPluginBase::bindSchema(type_name, schema);
      if (is_log) {
        registerLogHandler(type_name);
      }
      return PJ::okStatus();
    }

    gp::FileDescriptorSet fd_set;
    if (!fd_set.ParseFromArray(schema.data(), static_cast<int>(schema.size()))) {
      return PJ::unexpected(std::string("failed to parse FileDescriptorSet"));
    }

    // The pool keeps a pointer to its database: release it before replacing the database.
    descriptor_ = nullptr;
    factory_.reset();
    pool_.reset();
    database_ = std::make_unique<gp::SimpleDescriptorDatabase>();
    addSchemaFiles(fd_set, *database_);
    pool_ = std::make_unique<gp::DescriptorPool>(database_.get());
    factory_ = std::make_unique<gp::DynamicMessageFactory>(pool_.get());

    descriptor_ = pool_->FindMessageTypeByName(std::string(type_name));
    if (descriptor_ == nullptr) {
      return PJ::unexpected(std::string("message type not found: ") + std::string(type_name));
    }

    // Embedded timestamp: a top-level non-repeated double. With timestamp_field_name_ set only that name is tried,
    // otherwise "timestamp" then "ts" (same contract as parser_json).
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

    // Record the bound type for the handler dispatch, so a parser re-bound from a
    // Log schema to a generic one never dispatches to the stale Log handler.
    MessageParserPluginBase::bindSchema(type_name, schema);
    if (is_log) {
      registerLogHandler(type_name);
    }
    return PJ::okStatus();
  }

  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("write host not bound"));
    }
    // Canonical-object schemas have no descriptor pool; the base parse() dispatches to their registered SchemaHandler.
    if (descriptor_ == nullptr) {
      if (findSchemaHandler(bound_type_name_) != nullptr) {
        return MessageParserPluginBase::parse(timestamp_ns, payload);
      }
      return PJ::unexpected(std::string("no schema bound"));
    }

    auto msg = decodeAndFlatten(payload);
    if (!msg) {
      return PJ::unexpected(std::move(msg).error());
    }

    // Extract embedded timestamp if available (overrides the provided timestamp).
    // An omitted presence-tracked timestamp would read as 0 (1970): keep the provided one.
    const gp::Reflection* reflection = (*msg)->GetReflection();
    if (timestamp_field_ != nullptr &&
        (!timestamp_field_->has_presence() || reflection->HasField(**msg, timestamp_field_))) {
      double ts_seconds = reflection->GetDouble(**msg, timestamp_field_);
      timestamp_ns = static_cast<PJ::Timestamp>(ts_seconds * 1e9);
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
  // Flattens the payload into owned_fields_ (the generic scalar route).
  PJ::Expected<std::unique_ptr<gp::Message>> decodeAndFlatten(PJ::Span<const uint8_t> payload) {
    const gp::Message* prototype = factory_->GetPrototype(descriptor_);
    std::unique_ptr<gp::Message> msg(prototype->New());
    if (!msg->ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
      return PJ::unexpected("failed to deserialize protobuf message (" + std::to_string(payload.size()) + " bytes)");
    }

    owned_fields_.clear();
    flattenMessage(*msg, "", false, array_limit_.max_size, array_limit_.clamp(), owned_fields_);

    // flattenMessage's push_back may have reallocated the vector, invalidating earlier string_view pointers.
    for (auto& f : owned_fields_) {
      if (!f.owned_string.empty()) {
        f.value = std::string_view(f.owned_string);
      }
    }
    return msg;
  }

  // foxglove.Log / PJ.Log -> kLog. Unlike the other fast paths the descriptor pool is still built: the scalar route
  // uses the generic flatten (level, message, name, ... stay plottable), which skips strings of 100 bytes or more. The
  // object route decodes with the SDK codec (PJ.Log is wire-compatible with foxglove.Log) and keeps messages of any
  // length.
  void registerLogHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kLog;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      if (descriptor_ == nullptr) {
        return PJ::unexpected(std::string("no schema bound"));
      }
      if (auto msg = decodeAndFlatten(payload); !msg) {
        return PJ::unexpected(std::move(msg).error());
      }
      PJ::sdk::ScalarRecord record;
      // Same timestamp policy as the object route, so scalars and object share a timeline.
      if (use_embedded_timestamp_) {
        if (auto log = PJ::deserializeLog(payload.data(), payload.size()); log && log->timestamp_ns > 0) {
          record.ts = log->timestamp_ns;
        }
      }
      record.fields.reserve(owned_fields_.size());
      for (const auto& f : owned_fields_) {
        record.fields.push_back({.name = f.name, .value = f.value});
      }
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      auto log = PJ::deserializeLog(payload.bytes.data(), payload.bytes.size());
      if (!log) {
        return PJ::unexpected(std::move(log).error());
      }
      std::optional<PJ::Timestamp> ts;
      if (use_embedded_timestamp_ && log->timestamp_ns > 0) {
        ts = log->timestamp_ns;
      }
      return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*log)}};
    };

    registerSchemaHandler(type_name, std::move(handler));
  }

  // Object route: one PJ.VideoFrame / foxglove.CompressedVideo per message, zero-copy. Scalar route: a slim metadata
  // row (timestamp / frame_id / format / data size) so ingest still produces plottable columns.
  void registerVideoFrameHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kVideoFrame;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      // Owning decode, not the zero-copy view: the scalar route only needs the metadata strings and data size, and a
      // view here would alias `payload` with a null anchor (a use-after-free if the handler ever surfaces the bytes).
      auto frame = PJ::deserializeVideoFrame(payload.data(), payload.size());
      if (!frame) {
        return PJ::unexpected(std::move(frame).error());
      }
      // ValueRef holds a non-owning string_view and the host reads the fields after this lambda returns, so the
      // decoded strings are parked in member storage.
      video_frame_id_ = std::move(frame->frame_id);
      video_format_ = std::move(frame->format);
      PJ::sdk::ScalarRecord record;
      // Same timestamp policy as the object route; otherwise scalars land on host time while the object uses sensor
      // time.
      if (use_embedded_timestamp_ && frame->timestamp_ns > 0) {
        record.ts = frame->timestamp_ns;
      }
      record.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(frame->timestamp_ns)}});
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(video_frame_id_)}});
      record.fields.push_back({.name = "format", .value = PJ::sdk::ValueRef{std::string_view(video_format_)}});
      record.fields.push_back(
          {.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(frame->data.size())}});
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      // Zero-copy: the frame's data span aliases payload.bytes; the anchor keeps that buffer alive past this call.
      auto frame = PJ::deserializeVideoFrameView(payload.bytes.data(), payload.bytes.size(), payload.anchor);
      if (!frame) {
        return PJ::unexpected(std::move(frame).error());
      }
      std::optional<PJ::Timestamp> ts;
      if (use_embedded_timestamp_ && frame->timestamp_ns > 0) {
        ts = frame->timestamp_ns;
      }
      return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*frame)}};
    };

    registerSchemaHandler(type_name, std::move(handler));
  }

  // Object route: one PJ.PosesInFrame / foxglove.PosesInFrame per message via the SDK codec (owning: the message has
  // no byte blob to alias).
  void registerPosesInFrameHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kPosesInFrame;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      // Slim metadata row: a pose array can be huge (e.g. an AMCL particle cloud), so only the header pair
      // (timestamp/frame_id) and a bounded count are emitted, never per-pose columns. An object-bearing entry needs a
      // parse_scalars at all: without one the host's eager-scalar ingest aborts the push and drops the object.
      auto poses = PJ::deserializePosesInFrame(payload.data(), payload.size());
      if (!poses) {
        return PJ::unexpected(std::move(poses).error());
      }
      poses_frame_id_ = std::move(poses->frame_id);  // keep alive for the string_view below.
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_ && poses->timestamp_ns > 0) {
        record.ts = poses->timestamp_ns;
      }
      record.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(poses->timestamp_ns)}});
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(poses_frame_id_)}});
      record.fields.push_back(
          {.name = "num_poses", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(poses->poses.size())}});
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      auto poses = PJ::deserializePosesInFrame(payload.bytes.data(), payload.bytes.size());
      if (!poses) {
        return PJ::unexpected(std::move(poses).error());
      }
      std::optional<PJ::Timestamp> ts;
      if (use_embedded_timestamp_ && poses->timestamp_ns > 0) {
        ts = poses->timestamp_ns;
      }
      return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*poses)}};
    };

    registerSchemaHandler(type_name, std::move(handler));
  }

  // foxglove.Odometry -> kPosesInFrame (one pose). The object route decodes the single pose (of body_frame_id, in
  // frame_id).
  //
  // Unlike the sibling slim-count handlers (PosesInFrame/FrameTransform emit num_poses/num_transforms), the scalar
  // route emits the 7 per-axis pose columns: an Odometry always carries exactly ONE pose, so a count would be a
  // constant, and these are the values people plot (like the ROS nav_msgs/Odometry route). Velocities and covariances
  // are left out.
  void registerFoxgloveOdometryHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kPosesInFrame;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      auto poses = pj_protobuf::deserializeFoxgloveOdometry(payload.data(), payload.size(), odometry_fields_);
      if (!poses) {
        return PJ::unexpected(std::move(poses).error());
      }
      odometry_frame_id_ = std::move(poses->frame_id);  // keep alive for the string_view below.
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_ && poses->timestamp_ns > 0) {
        record.ts = poses->timestamp_ns;
      }
      record.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(poses->timestamp_ns)}});
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(odometry_frame_id_)}});
      if (!poses->poses.empty()) {
        const auto& p = poses->poses.front();
        record.fields.push_back({.name = "pose/position/x", .value = PJ::sdk::ValueRef{p.position.x}});
        record.fields.push_back({.name = "pose/position/y", .value = PJ::sdk::ValueRef{p.position.y}});
        record.fields.push_back({.name = "pose/position/z", .value = PJ::sdk::ValueRef{p.position.z}});
        record.fields.push_back({.name = "pose/orientation/x", .value = PJ::sdk::ValueRef{p.orientation.x}});
        record.fields.push_back({.name = "pose/orientation/y", .value = PJ::sdk::ValueRef{p.orientation.y}});
        record.fields.push_back({.name = "pose/orientation/z", .value = PJ::sdk::ValueRef{p.orientation.z}});
        record.fields.push_back({.name = "pose/orientation/w", .value = PJ::sdk::ValueRef{p.orientation.w}});
      }
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      auto poses =
          pj_protobuf::deserializeFoxgloveOdometry(payload.bytes.data(), payload.bytes.size(), odometry_fields_);
      if (!poses) {
        return PJ::unexpected(std::move(poses).error());
      }
      std::optional<PJ::Timestamp> ts;
      if (use_embedded_timestamp_ && poses->timestamp_ns > 0) {
        ts = poses->timestamp_ns;
      }
      return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*poses)}};
    };

    registerSchemaHandler(type_name, std::move(handler));
  }

  // Object route: one cloud per message, zero-copy (the packed-point span aliases the payload). Scalar route: a slim
  // metadata row (timestamp / frame_id / point_count) so ingest still produces plottable columns.
  void registerFoxglovePointCloudHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kPointCloud;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      // No anchor needed: only metadata is read, and the aliased point buffer is not retained past this call.
      auto decoded =
          pj_protobuf::deserializeFoxglovePointCloudView(payload.data(), payload.size(), nullptr, pointcloud_fields_);
      if (!decoded) {
        return PJ::unexpected(std::move(decoded).error());
      }
      const auto& cloud = decoded->cloud;
      pointcloud_frame_id_ = cloud.frame_id;  // keep alive for the string_view below.
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_ && cloud.timestamp_ns > 0) {
        record.ts = cloud.timestamp_ns;
      }
      record.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(cloud.timestamp_ns)}});
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(pointcloud_frame_id_)}});
      record.fields.push_back({.name = "point_count", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(cloud.width)}});
      // point_step and num_fields are layout constants (the object route carries them), not a time series.
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      auto decoded = pj_protobuf::deserializeFoxglovePointCloudView(
          payload.bytes.data(), payload.bytes.size(), payload.anchor, pointcloud_fields_);
      if (!decoded) {
        return PJ::unexpected(std::move(decoded).error());
      }
      // sdk::PointCloud expresses geometry in frame_id plus an external TF tree (like PointCloud2), so Foxglove's
      // inline pose has no home and a non-identity one is dropped. Warn once: the cloud is mislocated without an
      // equivalent TF.
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

  // Object route: decodes and eagerly projects one scan per message into an OWNED point buffer (not zero-copy: the
  // wire carries polar ranges, the cloud newly generated cartesian points, anchored by the projector). Scalar route: a
  // slim metadata row (timestamp / frame_id / start_angle / end_angle / num_ranges) from a header-only walk, with no
  // LUT and no projection.
  void registerFoxgloveLaserScanHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kPointCloud;

    handler.parse_scalars =
        [this](PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      auto info = pj_protobuf::readFoxgloveLaserScanInfo(payload.data(), payload.size(), laserscan_fields_);
      if (!info) {
        return PJ::unexpected(std::move(info).error());
      }
      laserscan_frame_id_ = std::move(info->frame_id);  // keep alive for the string_view below.
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_ && info->timestamp_ns > 0) {
        record.ts = info->timestamp_ns;
      }
      // Flat metadata names (no '/' prefix), like the PointCloud handler.
      record.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(info->timestamp_ns)}});
      record.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(laserscan_frame_id_)}});
      record.fields.push_back({.name = "start_angle", .value = PJ::sdk::ValueRef{info->start_angle}});
      record.fields.push_back({.name = "end_angle", .value = PJ::sdk::ValueRef{info->end_angle}});
      record.fields.push_back({.name = "num_ranges", .value = PJ::sdk::ValueRef{info->num_ranges}});
      return record;
    };

    handler.parse_object =
        [this](PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(
          payload.bytes.data(), payload.bytes.size(), laserscan_projector_, laserscan_fields_);
      if (!decoded) {
        return PJ::unexpected(std::move(decoded).error());
      }
      // Same as foxglove.PointCloud: sdk::PointCloud has no pose, so a non-identity one is dropped; warn once.
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

  // Resolves the field numbers of a well-known Foxglove schema from the bound topic's embedded FileDescriptorSet, so
  // the hand-rolled decoders honor a self-describing file that renumbered the schema (the dispatcher keys off the
  // schema NAME only). Builds a throwaway pool just to read field numbers by NAME; each codec keeps its default
  // numbering when there is no schema (streaming) or it cannot be parsed/built.
  void resolveFoxgloveFieldNumbers(std::string_view type_name, PJ::Span<const uint8_t> schema) {
    if (schema.empty()) {
      return;
    }
    gp::FileDescriptorSet fd_set;
    if (!fd_set.ParseFromArray(schema.data(), static_cast<int>(schema.size()))) {
      return;
    }
    gp::SimpleDescriptorDatabase database;
    addSchemaFiles(fd_set, database);
    gp::DescriptorPool pool(&database);
    const gp::Descriptor* descriptor = pool.FindMessageTypeByName(std::string(type_name));
    if (descriptor == nullptr) {
      // The schema was present but unbuildable (e.g. a dependency like google.protobuf.Timestamp missing from the set).
      // Falling back to the official numbers is reported because a renumbered schema would otherwise misparse silently.
      std::cerr << "[protobuf_parser] could not resolve a descriptor for " << type_name
                << " from its embedded schema; assuming official foxglove field numbers "
                   "(a renumbered schema would misparse).\n";
      return;
    }
    if (type_name == "foxglove.RawImage") {
      raw_image_fields_ = pj_protobuf::resolveRawImageFieldNumbers(descriptor);
    } else if (type_name == "foxglove.CompressedImage") {
      compressed_image_fields_ = pj_protobuf::resolveCompressedImageFieldNumbers(descriptor);
    } else if (type_name == "foxglove.CameraCalibration") {
      camera_calibration_fields_ = pj_protobuf::resolveCameraCalibrationFieldNumbers(descriptor);
    } else if (type_name == "foxglove.FrameTransform") {
      frame_transform_fields_ = pj_protobuf::resolveFrameTransformFieldNumbers(descriptor);
    } else if (type_name == "foxglove.Odometry") {
      odometry_fields_ = pj_protobuf::resolveOdometryFieldNumbers(descriptor);
    } else if (type_name == "foxglove.ImageAnnotations") {
      image_annotations_fields_ = pj_protobuf::resolveImageAnnotationsFieldNumbers(descriptor);
    } else if (type_name == "foxglove.SceneUpdate") {
      scene_update_fields_ = pj_protobuf::resolveSceneUpdateFieldNumbers(descriptor);
    } else if (type_name == "foxglove.PointCloud") {
      pointcloud_fields_ = pj_protobuf::resolvePointCloudFieldNumbers(descriptor);
    } else if (type_name == "foxglove.LaserScan") {
      laserscan_fields_ = pj_protobuf::resolveLaserScanFieldNumbers(descriptor);
    } else if (type_name == "foxglove.VoxelGrid") {
      voxelgrid_fields_ = pj_protobuf::resolveVoxelGridFieldNumbers(descriptor);
    } else if (type_name == "foxglove.Grid") {
      grid_fields_ = pj_protobuf::resolveGridFieldNumbers(descriptor);
    }
  }

  // parse_object decodes the canonical builtin object; parse_scalars emits a slim, BOUNDED metadata row (embedded
  // timestamp + frame_id where the schema has one, plus counts/sizes), avoiding the per-element scalar blow-up of the
  // generic flatten.
  void registerFoxgloveObjectHandler(std::string_view type_name) {
    PJ::sdk::SchemaHandler handler;
    const std::string name(type_name);

    if (name == "foxglove.FrameTransform") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kFrameTransforms;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj =
            pj_protobuf::deserializeFoxgloveFrameTransform(p.bytes.data(), p.bytes.size(), frame_transform_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && !obj->transforms.empty() && obj->transforms.front().timestamp_ns > 0) {
          ts = obj->transforms.front().timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        // FrameTransform is cheap to decode (a stamp, two frame ids, 7 doubles, no blob), so the embedded stamp and the
        // real count are emitted.
        auto obj =
            pj_protobuf::deserializeFoxgloveFrameTransform(payload.data(), payload.size(), frame_transform_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        PJ::sdk::ScalarRecord r;
        if (use_embedded_timestamp_ && !obj->transforms.empty() && obj->transforms.front().timestamp_ns > 0) {
          r.ts = obj->transforms.front().timestamp_ns;
        }
        // The schema holds exactly one transform, so "the" stamp is the first one's; with none decoded the series is
        // omitted rather than filed as a fake 0.
        if (!obj->transforms.empty()) {
          r.fields.push_back(
              {.name = "timestamp",
               .value = PJ::sdk::ValueRef{timestampSeconds(obj->transforms.front().timestamp_ns)}});
        }
        r.fields.push_back(
            {.name = "num_transforms", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->transforms.size())}});
        return r;
      };
    } else if (name == "foxglove.CompressedImage") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCompressedImageView(
            p.bytes.data(), p.bytes.size(), p.anchor, compressed_image_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCompressedImageView(
            payload.data(), payload.size(), nullptr, compressed_image_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        // The decoded strings are parked in members because ValueRef holds a non-owning string_view.
        // sdk::Image::encoding IS the foxglove `format` string verbatim, so the column keeps the schema's name.
        compressed_image_frame_id_ = std::move(obj->frame_id);
        compressed_image_format_ = std::move(obj->encoding);
        PJ::sdk::ScalarRecord r;
        // Same timestamp policy as the object route, so scalars and object share a timeline.
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          r.ts = obj->timestamp_ns;
        }
        r.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(obj->timestamp_ns)}});
        r.fields.push_back(
            {.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(compressed_image_frame_id_)}});
        r.fields.push_back({.name = "format", .value = PJ::sdk::ValueRef{std::string_view(compressed_image_format_)}});
        r.fields.push_back({.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->data.size())}});
        return r;
      };
    } else if (name == "foxglove.CameraCalibration") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kCameraInfo;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCameraCalibration(
            p.bytes.data(), p.bytes.size(), camera_calibration_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveCameraCalibration(
            payload.data(), payload.size(), camera_calibration_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        camera_calibration_frame_id_ = std::move(obj->frame_id);  // keep alive for the string_view below.
        PJ::sdk::ScalarRecord r;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {  // match the object route's timeline
          r.ts = obj->timestamp_ns;
        }
        r.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(obj->timestamp_ns)}});
        r.fields.push_back(
            {.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(camera_calibration_frame_id_)}});
        r.fields.push_back({.name = "width", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->width)}});
        r.fields.push_back({.name = "height", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->height)}});
        return r;
      };
    } else if (name == "foxglove.ImageAnnotations") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kImageAnnotations;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj =
            pj_protobuf::deserializeFoxgloveImageAnnotations(p.bytes.data(), p.bytes.size(), image_annotations_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj =
            pj_protobuf::deserializeFoxgloveImageAnnotations(payload.data(), payload.size(), image_annotations_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        PJ::sdk::ScalarRecord r;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {  // match the object route's timeline
          r.ts = obj->timestamp_ns;
        }
        // No top-level frame_id: annotations are tied to an image by layer stacking, not by frame.
        r.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(obj->timestamp_ns)}});
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
        auto obj =
            pj_protobuf::deserializeFoxgloveRawImageView(p.bytes.data(), p.bytes.size(), p.anchor, raw_image_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj =
            pj_protobuf::deserializeFoxgloveRawImageView(payload.data(), payload.size(), nullptr, raw_image_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        raw_image_frame_id_ = std::move(obj->frame_id);  // keep alive for the string_view below.
        PJ::sdk::ScalarRecord r;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {  // match the object route's timeline
          r.ts = obj->timestamp_ns;
        }
        r.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(obj->timestamp_ns)}});
        r.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(raw_image_frame_id_)}});
        r.fields.push_back({.name = "width", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->width)}});
        r.fields.push_back({.name = "height", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->height)}});
        r.fields.push_back({.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->data.size())}});
        return r;
      };
    } else if (name == "foxglove.VoxelGrid") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kVoxelGrid;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj =
            pj_protobuf::deserializeFoxgloveVoxelGridView(p.bytes.data(), p.bytes.size(), p.anchor, voxelgrid_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj =
            pj_protobuf::deserializeFoxgloveVoxelGridView(payload.data(), payload.size(), nullptr, voxelgrid_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        voxelgrid_frame_id_ = std::move(obj->frame_id);  // keep alive for the string_view below.
        PJ::sdk::ScalarRecord r;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {  // match the object route's timeline
          r.ts = obj->timestamp_ns;
        }
        r.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(obj->timestamp_ns)}});
        r.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(voxelgrid_frame_id_)}});
        r.fields.push_back(
            {.name = "column_count", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->column_count)}});
        r.fields.push_back({.name = "row_count", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->row_count)}});
        r.fields.push_back(
            {.name = "slice_count", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->slice_count)}});
        r.fields.push_back({.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->data.size())}});
        return r;
      };
    } else if (name == "foxglove.Grid") {
      handler.object_type = PJ::sdk::BuiltinObjectType::kGridMap;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveGridView(p.bytes.data(), p.bytes.size(), p.anchor, grid_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {
          ts = obj->timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveGridView(payload.data(), payload.size(), nullptr, grid_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        grid_frame_id_ = std::move(obj->frame_id);  // keep alive for the string_view below.
        PJ::sdk::ScalarRecord r;
        if (use_embedded_timestamp_ && obj->timestamp_ns > 0) {  // match the object route's timeline
          r.ts = obj->timestamp_ns;
        }
        r.fields.push_back({.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(obj->timestamp_ns)}});
        r.fields.push_back({.name = "frame_id", .value = PJ::sdk::ValueRef{std::string_view(grid_frame_id_)}});
        r.fields.push_back(
            {.name = "column_count", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->column_count)}});
        r.fields.push_back({.name = "row_count", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->row_count)}});
        r.fields.push_back({.name = "row_stride", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->row_stride)}});
        r.fields.push_back(
            {.name = "cell_stride", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->cell_stride)}});
        r.fields.push_back({.name = "data_size", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->data.size())}});
        return r;
      };
    } else {  // foxglove.SceneUpdate
      handler.object_type = PJ::sdk::BuiltinObjectType::kSceneEntities;
      handler.parse_object = [this](PJ::Timestamp, PJ::sdk::PayloadView p) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveSceneUpdate(p.bytes.data(), p.bytes.size(), scene_update_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        std::optional<PJ::Timestamp> ts;
        if (use_embedded_timestamp_ && !obj->entities.empty() && obj->entities.front().timestamp_ns > 0) {
          ts = obj->entities.front().timestamp_ns;
        }
        return PJ::sdk::ObjectRecord{.ts = ts, .object = PJ::sdk::BuiltinObject{std::move(*obj)}};
      };
      handler.parse_scalars =
          [this](PJ::Timestamp, PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        auto obj = pj_protobuf::deserializeFoxgloveSceneUpdate(payload.data(), payload.size(), scene_update_fields_);
        if (!obj) {
          return PJ::unexpected(std::move(obj).error());
        }
        PJ::sdk::ScalarRecord r;
        if (use_embedded_timestamp_ && !obj->entities.empty() && obj->entities.front().timestamp_ns > 0) {
          r.ts = obj->entities.front().timestamp_ns;  // match the object route's timeline
        }
        // A SceneUpdate has no top-level stamp; the first entity's is the one the object route files the record under.
        // With no entities the column is omitted rather than filed as a fabricated 0.0 (same as FrameTransform).
        if (!obj->entities.empty()) {
          r.fields.push_back(
              {.name = "timestamp", .value = PJ::sdk::ValueRef{timestampSeconds(obj->entities.front().timestamp_ns)}});
        }
        r.fields.push_back(
            {.name = "num_entities", .value = PJ::sdk::ValueRef{static_cast<uint64_t>(obj->entities.size())}});
        return r;
      };
    }

    registerSchemaHandler(type_name, std::move(handler));
  }

  // Declared in dependency order: the factory uses the pool, which builds from the database.
  std::unique_ptr<gp::SimpleDescriptorDatabase> database_;
  std::unique_ptr<gp::DescriptorPool> pool_;
  std::unique_ptr<gp::DynamicMessageFactory> factory_;
  const gp::Descriptor* descriptor_ = nullptr;
  const gp::FieldDescriptor* timestamp_field_ = nullptr;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_;  // empty = fallback chain ("timestamp" → "ts")
  PJ::sdk::ArrayLimit array_limit_;

  // Field numbers for the well-known Foxglove schemas, resolved from the bound topic's embedded descriptor in
  // bindSchema(). Schemaless streams keep each codec's defaults; a renumbered schema overrides only the named fields.
  pj_protobuf::RawImageFieldNumbers raw_image_fields_;
  pj_protobuf::CompressedImageFieldNumbers compressed_image_fields_;
  pj_protobuf::CameraCalibrationFieldNumbers camera_calibration_fields_;
  pj_protobuf::FrameTransformFieldNumbers frame_transform_fields_;
  pj_protobuf::OdometryFieldNumbers odometry_fields_;
  pj_protobuf::ImageAnnotationsFieldNumbers image_annotations_fields_;
  pj_protobuf::SceneUpdateFieldNumbers scene_update_fields_;
  pj_protobuf::PointCloudFieldNumbers pointcloud_fields_;
  pj_protobuf::LaserScanFieldNumbers laserscan_fields_;
  pj_protobuf::VoxelGridFieldNumbers voxelgrid_fields_;
  pj_protobuf::GridFieldNumbers grid_fields_;

  std::unordered_map<std::string, PJ::sdk::FieldHandle> field_cache_;
  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::BoundFieldValue> bound_fields_;

  // Scalar-route string storage: ValueRef holds a non-owning string_view, so every decoded string a ScalarRecord
  // references must outlive the lambda that decoded it. One parser instance serves one schema, so these are mutually
  // exclusive in practice.
  std::string video_frame_id_;
  std::string video_format_;

  // The latch makes the dropped-pose warning fire once per parser instance.
  std::string pointcloud_frame_id_;
  bool pointcloud_pose_warned_ = false;

  // One projector per parser instance (= per topic), so the cos/sin LUT is computed once per scanner config.
  PJ::laser_scan::LaserScanProjector laserscan_projector_;
  std::string laserscan_frame_id_;
  bool laserscan_pose_warned_ = false;

  std::string poses_frame_id_;
  std::string odometry_frame_id_;
  std::string compressed_image_frame_id_;
  std::string compressed_image_format_;
  std::string raw_image_frame_id_;
  std::string camera_calibration_frame_id_;
  std::string voxelgrid_frame_id_;
  std::string grid_frame_id_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ProtobufParser, kProtobufManifest)

PJ_DIALOG_PLUGIN(ProtobufParserDialog, kProtobufManifest)
