#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "protobuf_manifest.hpp"
#include "protobuf_parser_options_ui.hpp"

#include <nlohmann/json.hpp>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace gp = google::protobuf;

/// Error collector for protobuf compiler (stores errors as strings).
class ProtoErrorCollector : public gp::compiler::MultiFileErrorCollector {
 public:
#if GOOGLE_PROTOBUF_VERSION >= 4022000
  void RecordError(absl::string_view filename, int line, int /*column*/, absl::string_view message) override {
    errors_.push_back(std::string(filename) + ":" + std::to_string(line) + ": " + std::string(message));
  }
  void RecordWarning(absl::string_view /*filename*/, int /*line*/, int /*column*/,
                     absl::string_view /*message*/) override {}
#else
  void AddError(const std::string& filename, int line, int /*column*/, const std::string& message) override {
    errors_.push_back(filename + ":" + std::to_string(line) + ": " + message);
  }
  void AddWarning(const std::string& /*filename*/, int /*line*/, int /*column*/,
                  const std::string& /*message*/) override {}
#endif

  const std::vector<std::string>& errors() const { return errors_; }
  void clear() { errors_.clear(); }

 private:
  std::vector<std::string> errors_;
};

/// Dialog plugin for the Protobuf Parser options.
/// Provides UI for loading .proto files, selecting message types, and include folders.
/// Compiles the .proto file using google::protobuf::compiler::Importer and serializes
/// the FileDescriptorSet for use by the parser.
class ProtobufParserDialog : public PJ::DialogPluginTyped {
 public:
  // --- Dialog protocol ---

  std::string manifest() const override { return kProtobufManifest; }

  std::string ui_content() const override { return kProtobufParserOptionsUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Proto file path display
    wd.setText("labelProtoFilePath",
               proto_file_path_.empty() ? "(no file selected)" : filenameFromPath(proto_file_path_));

    // Proto file picker button
    wd.setFilePicker("buttonLoadProtoFile", "Load .proto file", "*.proto", "Select Proto File");

    // Message type combo
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

    // Show compilation error if any
    if (!compile_error_.empty()) {
      wd.setPlainText("textEditProtoPreview", "COMPILE ERROR:\n" + compile_error_ + "\n\n" + proto_file_content_);
    } else {
      wd.setPlainText("textEditProtoPreview", proto_file_content_);
    }

    // Timestamp checkbox
    wd.setChecked("checkBoxUseEmbeddedTimestamp", use_embedded_timestamp_);

    // Include folders - folder picker
    wd.setFolderPicker("buttonAddIncludeFolder", "Add folder...", "Select Include Folder");

    // Include folders list
    wd.setListItems("listWidgetIncludeFolders", include_folders_);

    // Enable remove button if there are folders and selection
    bool can_remove = !include_folders_.empty() && !selected_include_folders_.empty();
    wd.setEnabled("buttonRemoveIncludeFolder", can_remove);

    return wd.toJson();
  }

  bool onFileSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonLoadProtoFile") {
      proto_file_path_ = std::string(path);
      loadAndCompileProtoFile();
      return true;  // Refresh UI
    }
    return false;
  }

  bool onFolderSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonAddIncludeFolder") {
      std::string folder(path);
      // Avoid duplicates
      if (std::find(include_folders_.begin(), include_folders_.end(), folder) == include_folders_.end()) {
        include_folders_.push_back(folder);
        // Re-compile with new include path
        if (!proto_file_path_.empty()) {
          loadAndCompileProtoFile();
        }
      }
      return true;  // Refresh UI
    }
    return false;
  }

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBoxMessageType") {
      if (index >= 0 && index < static_cast<int>(message_types_.size())) {
        selected_message_type_ = message_types_[static_cast<size_t>(index)];
      }
      return false;  // No UI refresh needed
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxUseEmbeddedTimestamp") {
      use_embedded_timestamp_ = checked;
      return false;
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonRemoveIncludeFolder") {
      // Remove selected folders
      for (const auto& sel : selected_include_folders_) {
        auto it = std::find(include_folders_.begin(), include_folders_.end(), sel);
        if (it != include_folders_.end()) {
          include_folders_.erase(it);
        }
      }
      selected_include_folders_.clear();
      // Re-compile with updated include paths
      if (!proto_file_path_.empty()) {
        loadAndCompileProtoFile();
      }
      return true;  // Refresh UI
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name == "listWidgetIncludeFolders") {
      selected_include_folders_ = selected;
      return true;  // Refresh UI to update remove button state
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["proto_file_path"] = proto_file_path_;
    cfg["message_type"] = selected_message_type_;
    cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["include_folders"] = include_folders_;

    // Include the compiled schema as base64-encoded bytes
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
    proto_file_path_ = cfg.value("proto_file_path", std::string{});
    selected_message_type_ = cfg.value("message_type", std::string{});
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);

    include_folders_.clear();
    if (cfg.contains("include_folders") && cfg["include_folders"].is_array()) {
      for (const auto& f : cfg["include_folders"]) {
        if (f.is_string()) include_folders_.push_back(f.get<std::string>());
      }
    }

    // Load compiled schema if present
    if (cfg.contains("compiled_schema_base64") && cfg["compiled_schema_base64"].is_string()) {
      compiled_schema_ = base64Decode(cfg["compiled_schema_base64"].get<std::string>());
    }

    // Reload and recompile proto file
    if (!proto_file_path_.empty()) {
      loadAndCompileProtoFile();
    }

    return true;
  }

 private:
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

    if (proto_file_path_.empty()) return;

    // Read file content for preview
    std::ifstream file(proto_file_path_);
    if (!file.is_open()) {
      compile_error_ = "Could not open file: " + proto_file_path_;
      return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    proto_file_content_ = buffer.str();
    file.close();

    // Setup protobuf compiler
    gp::compiler::DiskSourceTree source_tree;

    // Map paths for imports
    source_tree.MapPath("", "");
    source_tree.MapPath("/", "/");

    // Add the directory containing the proto file
    std::string proto_dir = directoryFromPath(proto_file_path_);
    source_tree.MapPath("", proto_dir);

    // Add user-specified include folders
    for (const auto& folder : include_folders_) {
      source_tree.MapPath("", folder);
    }

    ProtoErrorCollector error_collector;
    gp::compiler::Importer importer(&source_tree, &error_collector);

    // Import the proto file
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

    // Extract message types
    for (int i = 0; i < file_descriptor->message_type_count(); i++) {
      const gp::Descriptor* msg_desc = file_descriptor->message_type(i);
      message_types_.push_back(std::string(msg_desc->full_name()));
    }

    // Build FileDescriptorSet containing all dependencies
    gp::FileDescriptorSet fd_set;
    buildFileDescriptorSet(file_descriptor, fd_set);

    // Serialize to bytes
    compiled_schema_ = fd_set.SerializeAsString();

    // If selected message type is not in the list, clear it
    if (!selected_message_type_.empty()) {
      if (std::find(message_types_.begin(), message_types_.end(), selected_message_type_) == message_types_.end()) {
        selected_message_type_.clear();
      }
    }

    // Default to first message type if none selected
    if (selected_message_type_.empty() && !message_types_.empty()) {
      selected_message_type_ = message_types_[0];
    }
  }

  /// Recursively add file descriptors and their dependencies to the set
  void buildFileDescriptorSet(const gp::FileDescriptor* fd, gp::FileDescriptorSet& fd_set) {
    // Check if already added
    for (int i = 0; i < fd_set.file_size(); i++) {
      if (fd_set.file(i).name() == fd->name()) {
        return;
      }
    }

    // Add dependencies first
    for (int i = 0; i < fd->dependency_count(); i++) {
      buildFileDescriptorSet(fd->dependency(i), fd_set);
    }

    // Add this file
    fd->CopyTo(fd_set.add_file());
  }

  int messageTypeIndex() const {
    if (selected_message_type_.empty() || message_types_.empty()) return 0;
    auto it = std::find(message_types_.begin(), message_types_.end(), selected_message_type_);
    return (it != message_types_.end()) ? static_cast<int>(std::distance(message_types_.begin(), it)) : 0;
  }

  // Base64 encoding/decoding for serialized schema
  static std::string base64Encode(const std::string& input) {
    static const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
      uint32_t n = (static_cast<uint8_t>(input[i]) << 16);
      if (i + 1 < input.size()) n |= (static_cast<uint8_t>(input[i + 1]) << 8);
      if (i + 2 < input.size()) n |= static_cast<uint8_t>(input[i + 2]);

      output.push_back(kBase64Chars[(n >> 18) & 0x3F]);
      output.push_back(kBase64Chars[(n >> 12) & 0x3F]);
      output.push_back((i + 1 < input.size()) ? kBase64Chars[(n >> 6) & 0x3F] : '=');
      output.push_back((i + 2 < input.size()) ? kBase64Chars[n & 0x3F] : '=');
    }
    return output;
  }

  static std::string base64Decode(const std::string& input) {
    static const int kDecodeTable[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55,
        56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
        13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32,
        33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    };

    std::string output;
    output.reserve((input.size() / 4) * 3);

    for (size_t i = 0; i < input.size(); i += 4) {
      int n0 = kDecodeTable[static_cast<uint8_t>(input[i])];
      int n1 = (i + 1 < input.size()) ? kDecodeTable[static_cast<uint8_t>(input[i + 1])] : 0;
      int n2 = (i + 2 < input.size() && input[i + 2] != '=') ? kDecodeTable[static_cast<uint8_t>(input[i + 2])] : 0;
      int n3 = (i + 3 < input.size() && input[i + 3] != '=') ? kDecodeTable[static_cast<uint8_t>(input[i + 3])] : 0;

      if (n0 < 0 || n1 < 0) continue;

      output.push_back(static_cast<char>((n0 << 2) | (n1 >> 4)));
      if (i + 2 < input.size() && input[i + 2] != '=') {
        output.push_back(static_cast<char>(((n1 & 0x0F) << 4) | (n2 >> 2)));
      }
      if (i + 3 < input.size() && input[i + 3] != '=') {
        output.push_back(static_cast<char>(((n2 & 0x03) << 6) | n3));
      }
    }
    return output;
  }

  // Persistent config
  std::string proto_file_path_;
  std::string selected_message_type_;
  bool use_embedded_timestamp_ = false;
  std::vector<std::string> include_folders_;
  std::string compiled_schema_;  // Serialized FileDescriptorSet

  // Transient state (not saved to config)
  std::string proto_file_content_;
  std::vector<std::string> message_types_;
  std::vector<std::string> selected_include_folders_;
  std::string compile_error_;
};

}  // namespace
