#pragma once

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base64/base64.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <pj_utf8_path/utf8_path.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "protobuf_manifest.hpp"
#include "protobuf_parser_options_ui.hpp"

namespace {

namespace gp = google::protobuf;

class ProtoErrorCollector : public gp::compiler::MultiFileErrorCollector {
 public:
#if GOOGLE_PROTOBUF_VERSION >= 4022000
  void RecordError(absl::string_view filename, int line, int /*column*/, absl::string_view message) override {
    errors_.push_back(std::string(filename) + ":" + std::to_string(line) + ": " + std::string(message));
  }
  void RecordWarning(
      absl::string_view /*filename*/, int /*line*/, int /*column*/, absl::string_view /*message*/) override {}
#else
  void AddError(const std::string& filename, int line, int /*column*/, const std::string& message) override {
    errors_.push_back(filename + ":" + std::to_string(line) + ": " + message);
  }
  void AddWarning(
      const std::string& /*filename*/, int /*line*/, int /*column*/, const std::string& /*message*/) override {}
#endif

  const std::vector<std::string>& errors() const {
    return errors_;
  }
  void clear() {
    errors_.clear();
  }

 private:
  std::vector<std::string> errors_;
};

/// Loads a .proto, compiles it with google::protobuf::compiler::Importer and hands the serialized FileDescriptorSet to
/// the parser.
class ProtobufParserDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  std::string manifest() const override {
    return kProtobufManifest;
  }

  std::string ui_content() const override {
    return kProtobufParserOptionsUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Editable path field (typing a path == picking one) plus an icon-only browse button.
    wd.setText("lineEditProtoFile", proto_file_path_);
    wd.setFilePicker("buttonLoadProtoFile", "", "*.proto", "Select Proto File");
    wd.setButtonIconNamed("buttonLoadProtoFile", "file");

    bool has_file = !proto_file_path_.empty() && !message_types_.empty();
    wd.setEnabled("comboBoxMessageType", has_file);
    if (has_file) {
      wd.setItems("comboBoxMessageType", message_types_);
      int idx = messageTypeIndex();
      wd.setCurrentIndex("comboBoxMessageType", idx);
    } else {
      wd.setItems("comboBoxMessageType", {"(load a .proto file first)"});
      wd.setCurrentIndex("comboBoxMessageType", 0);
    }

    // Compile errors say when a saved descriptor remains usable, and a missing native source file is a non-fatal
    // notice rather than being silently treated as available.
    if (!compile_error_.empty()) {
      const std::string heading = !compiled_schema_.empty() && !message_types_.empty()
                                      ? "SOURCE COMPILE ERROR (USING SAVED DESCRIPTOR):\n"
                                      : "COMPILE ERROR:\n";
      wd.setPlainText("textEditProtoPreview", heading + compile_error_ + "\n\n" + proto_file_content_);
    } else if (!source_notice_.empty()) {
      wd.setPlainText("textEditProtoPreview", "SOURCE NOTICE:\n" + source_notice_ + "\n\n" + proto_file_content_);
    } else {
      wd.setPlainText("textEditProtoPreview", proto_file_content_);
    }

    wd.setChecked("checkBoxUseEmbeddedTimestamp", use_embedded_timestamp_);
    wd.setText("lineEditTimestampField", timestamp_field_name_);
    wd.setEnabled("lineEditTimestampField", use_embedded_timestamp_);
    wd.setEnabled("labelTimestampField", use_embedded_timestamp_);

    wd.setValue("spinBoxArraySize", static_cast<int>(array_limit_.max_size));
    wd.setChecked("radioMaxClamp", array_limit_.clamp());
    wd.setChecked("radioMaxDiscard", !array_limit_.clamp());

#ifdef PJ_TARGET_WASM
    // Browser-staged files do not expose stable host directories that protoc can traverse, so the include-folder
    // controls are inert.
    wd.setEnabled("includeFoldersColumn", false);
#endif
    wd.setFolderPicker("buttonAddIncludeFolder", "", "Select Include Folder");

    wd.setListItems("listWidgetIncludeFolders", include_folders_);
    wd.setListPlaceholder("listWidgetIncludeFolders", "Add folders where imported .proto files can be found:");
    // A per-row trash icon fires onItemDeleteRequested.
    wd.setListItemsDeletable("listWidgetIncludeFolders", true);

    return wd.toJson();
  }

  bool onFileSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonLoadProtoFile") {
      proto_file_path_ = std::string(path);
      loadAndCompileProtoFile();
      return true;
    }
    return false;
  }

  bool onFolderSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonAddIncludeFolder") {
      std::string folder(path);
      if (std::find(include_folders_.begin(), include_folders_.end(), folder) == include_folders_.end()) {
        include_folders_.push_back(folder);
        if (!proto_file_path_.empty()) {
          loadAndCompileProtoFile();
        }
      }
      return true;
    }
    return false;
  }

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBoxMessageType") {
      if (index >= 0 && index < static_cast<int>(message_types_.size())) {
        selected_message_type_ = message_types_[static_cast<size_t>(index)];
      }
      return false;
    }
    return false;
  }

  bool onValueChanged(std::string_view widget_name, int value) override {
    if (widget_name == "spinBoxArraySize") {
      array_limit_.max_size = static_cast<uint32_t>(value);
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxUseEmbeddedTimestamp") {
      use_embedded_timestamp_ = checked;
      return true;  // refresh to enable/disable lineEditTimestampField
    }
    if (checked && widget_name == "radioMaxClamp") {
      array_limit_.policy = PJ::sdk::ArrayPolicy::kClamp;
      return false;
    }
    if (checked && widget_name == "radioMaxDiscard") {
      array_limit_.policy = PJ::sdk::ArrayPolicy::kSkip;
      return false;
    }
    return false;
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditTimestampField") {
      timestamp_field_name_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditProtoFile") {
      proto_file_path_ = std::string(text);
      // Compile once the typed path resolves to an existing file. Otherwise (cleared or mistyped) the previously
      // compiled schema/types/preview are DROPPED, because saveConfig() would pair the new invalid path with the
      // stale schema. Clearing by hand rather than via loadAndCompileProtoFile avoids flashing "could not open" errors
      // on intermediate keystrokes.
      if (std::ifstream(PJ::utf8_path::pathFromUtf8(proto_file_path_)).good()) {
        loadAndCompileProtoFile();
      } else {
        message_types_.clear();
        proto_file_content_.clear();
        compiled_schema_.clear();
        compile_error_.clear();
        source_notice_.clear();
        selected_message_type_.clear();
      }
      return true;
    }
    return false;
  }

  bool onItemDeleteRequested(std::string_view widget_name, int index) override {
    if (widget_name == "listWidgetIncludeFolders" && index >= 0 && index < static_cast<int>(include_folders_.size())) {
      include_folders_.erase(include_folders_.begin() + index);
      if (!proto_file_path_.empty()) {
        loadAndCompileProtoFile();
      }
      return true;
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["proto_file_path"] = proto_file_path_;
    cfg["message_type"] = selected_message_type_;
    cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["timestamp_field_name"] = timestamp_field_name_;
    cfg["include_folders"] = include_folders_;
    PJ::sdk::arrayLimitToJson(cfg, array_limit_);

    if (!compiled_schema_.empty()) {
      cfg["compiled_schema_base64"] = base64Encode(compiled_schema_);
    }

    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    proto_file_content_.clear();
    compiled_schema_.clear();
    message_types_.clear();
    compile_error_.clear();
    source_notice_.clear();

    proto_file_path_ = cfg.value("proto_file_path", std::string{});
    selected_message_type_ = cfg.value("message_type", std::string{});
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    timestamp_field_name_ = cfg.value("timestamp_field_name", std::string{});
    array_limit_ = PJ::sdk::arrayLimitFromJson(cfg);

    include_folders_.clear();
    if (cfg.contains("include_folders") && cfg["include_folders"].is_array()) {
      for (const auto& f : cfg["include_folders"]) {
        if (f.is_string()) {
          include_folders_.push_back(f.get<std::string>());
        }
      }
    }

    if (cfg.contains("compiled_schema_base64") && cfg["compiled_schema_base64"].is_string()) {
      compiled_schema_ = PJ::base64::decode(cfg["compiled_schema_base64"].get<std::string>());
    }

    source_notice_.clear();

    // Restore the embedded descriptor first: the original .proto may have been moved, deleted or browser-staged at a
    // temporary path since the config was saved, so the descriptor set is the portable source of truth. A fresh
    // compilation is preferred only when the configured source is still available.
    restoreMessageTypesFromCompiledSchema();
    if (!proto_file_path_.empty()) {
      std::ifstream proto_file(PJ::utf8_path::pathFromUtf8(proto_file_path_));
      if (proto_file.good()) {
        proto_file.close();

        // An openable path may not be a compilable proto (a directory, empty/invalid, missing imports), so the
        // portable descriptor stays live if refreshing from the source fails.
        const std::string embedded_schema = compiled_schema_;
        const std::vector<std::string> embedded_message_types = message_types_;
        const std::string embedded_selected_message_type = selected_message_type_;
        loadAndCompileProtoFile();
        if (!compile_error_.empty() && !embedded_schema.empty() && !embedded_message_types.empty()) {
          compiled_schema_ = embedded_schema;
          message_types_ = embedded_message_types;
          selected_message_type_ = embedded_selected_message_type;
        }
      } else if (!message_types_.empty()) {
#ifndef PJ_TARGET_WASM
        source_notice_ = "Source file is unavailable; using the saved embedded descriptor: " + proto_file_path_;
#endif
      } else {
        compile_error_ = "Could not open file: " + proto_file_path_;
      }
    }

    return true;
  }

 private:
  void restoreMessageTypesFromCompiledSchema() {
    message_types_.clear();
    if (compiled_schema_.empty()) {
      return;
    }
    gp::FileDescriptorSet descriptors;
    if (!descriptors.ParseFromString(compiled_schema_) || descriptors.file().empty()) {
      return;
    }
    // buildFileDescriptorSet() appends dependencies first and the selected source file last. Like
    // loadAndCompileProtoFile(), list only that file's top-level messages, not those of imported dependencies.
    const gp::FileDescriptorProto& source = descriptors.file(descriptors.file_size() - 1);
    const std::string prefix = source.package().empty() ? std::string{} : source.package() + ".";
    for (const gp::DescriptorProto& message : source.message_type()) {
      message_types_.push_back(prefix + message.name());
    }
    // Same selection rule as loadAndCompileProtoFile(): drop a selection the schema no longer offers, then default to
    // the first type.
    if (!selected_message_type_.empty() &&
        std::find(message_types_.begin(), message_types_.end(), selected_message_type_) == message_types_.end()) {
      selected_message_type_.clear();
    }
    if (selected_message_type_.empty() && !message_types_.empty()) {
      selected_message_type_ = message_types_.front();
    }
  }

  static std::string filenameFromPath(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
  }

  static std::string directoryFromPath(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos != std::string::npos) ? path.substr(0, pos) : ".";
  }

  void loadAndCompileProtoFile() {
    message_types_.clear();
    proto_file_content_.clear();
    compiled_schema_.clear();
    compile_error_.clear();
    source_notice_.clear();

    if (proto_file_path_.empty()) {
      return;
    }

    std::ifstream file(PJ::utf8_path::pathFromUtf8(proto_file_path_));
    if (!file.is_open()) {
      compile_error_ = "Could not open file: " + proto_file_path_;
      return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    proto_file_content_ = buffer.str();
    file.close();

    gp::compiler::DiskSourceTree source_tree;

    // Import roots are the selected proto's own directory plus the configured include folders. Deliberately NOT the
    // filesystem root: mapping "/" would resolve imports against arbitrary host contents outside those roots.
    source_tree.MapPath("", "");

    std::string proto_dir = directoryFromPath(proto_file_path_);
    source_tree.MapPath("", proto_dir);

    for (const auto& folder : include_folders_) {
      source_tree.MapPath("", folder);
    }

    ProtoErrorCollector error_collector;
    gp::compiler::Importer importer(&source_tree, &error_collector);

    std::string filename = filenameFromPath(proto_file_path_);
    const gp::FileDescriptor* file_descriptor = importer.Import(filename);

    if (file_descriptor == nullptr) {
      if (!error_collector.errors().empty()) {
        compile_error_ = error_collector.errors().front();
      } else {
        compile_error_ = "Failed to compile proto file (unknown error)";
      }
      return;
    }

    for (int i = 0; i < file_descriptor->message_type_count(); i++) {
      const gp::Descriptor* msg_desc = file_descriptor->message_type(i);
      message_types_.push_back(std::string(msg_desc->full_name()));
    }

    gp::FileDescriptorSet fd_set;
    buildFileDescriptorSet(file_descriptor, fd_set);

    compiled_schema_ = fd_set.SerializeAsString();

    if (!selected_message_type_.empty()) {
      if (std::find(message_types_.begin(), message_types_.end(), selected_message_type_) == message_types_.end()) {
        selected_message_type_.clear();
      }
    }

    if (selected_message_type_.empty() && !message_types_.empty()) {
      selected_message_type_ = message_types_[0];
    }
  }

  void buildFileDescriptorSet(const gp::FileDescriptor* fd, gp::FileDescriptorSet& fd_set) {
    for (int i = 0; i < fd_set.file_size(); i++) {
      if (fd_set.file(i).name() == fd->name()) {
        return;
      }
    }

    // Add dependencies first
    for (int i = 0; i < fd->dependency_count(); i++) {
      buildFileDescriptorSet(fd->dependency(i), fd_set);
    }

    fd->CopyTo(fd_set.add_file());
  }

  int messageTypeIndex() const {
    if (selected_message_type_.empty() || message_types_.empty()) {
      return 0;
    }
    auto it = std::find(message_types_.begin(), message_types_.end(), selected_message_type_);
    return (it != message_types_.end()) ? static_cast<int>(std::distance(message_types_.begin(), it)) : 0;
  }

  static std::string base64Encode(const std::string& input) {
    static const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
      uint32_t n = static_cast<uint32_t>(static_cast<uint8_t>(input[i])) << 16;
      if (i + 1 < input.size()) {
        n |= static_cast<uint32_t>(static_cast<uint8_t>(input[i + 1])) << 8;
      }
      if (i + 2 < input.size()) {
        n |= static_cast<uint32_t>(static_cast<uint8_t>(input[i + 2]));
      }

      output.push_back(kBase64Chars[(n >> 18) & 0x3F]);
      output.push_back(kBase64Chars[(n >> 12) & 0x3F]);
      output.push_back((i + 1 < input.size()) ? kBase64Chars[(n >> 6) & 0x3F] : '=');
      output.push_back((i + 2 < input.size()) ? kBase64Chars[n & 0x3F] : '=');
    }
    return output;
  }

  std::string proto_file_path_;
  std::string selected_message_type_;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_;  // empty = fallback chain ("timestamp" → "ts")
  PJ::sdk::ArrayLimit array_limit_;
  std::vector<std::string> include_folders_;
  std::string compiled_schema_;  // Serialized FileDescriptorSet

  // Transient state (not saved to config)
  std::string proto_file_content_;
  std::vector<std::string> message_types_;
  std::string compile_error_;
  std::string source_notice_;
};

}  // namespace
