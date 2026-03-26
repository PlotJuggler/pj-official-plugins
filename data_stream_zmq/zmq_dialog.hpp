#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "datastream_zmq_ui.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace {

/// Dialog plugin for the ZMQ Subscriber.
class ZmqDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"ZMQ Subscriber","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kDataStreamZmqUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection mode
    wd.setChecked("radioConnect", connect_mode_);
    wd.setChecked("radioBind", !connect_mode_);

    // Transport combo
    wd.setItems("comboBox", {"tcp://", "ipc://", "pgm://"});
    wd.setCurrentIndex("comboBox", transportToIndex(transport_));

    // Address and port
    wd.setText("lineEditAddress", address_);
    wd.setText("lineEditPort", std::to_string(port_));

    // Protocol combo: use merged list when host has injected available_encodings,
    // otherwise fall back to the full known list.
    const auto& encs = encodings_.empty() ? kKnownEncodings() : encodings_;
    wd.setItems("comboBoxProtocol", encs);
    wd.setCurrentIndex("comboBoxProtocol", encodingToIndex(encoding_, encs));

    // Per-protocol options section
    bool has_options = hasOptions(encoding_);
    wd.setVisible("boxOptions", has_options);

    if (has_options) {
      bool is_protobuf = (encoding_ == "protobuf");

      // Simple timestamp checkbox for json/bson/msgpack
      wd.setVisible("checkBoxTimestamp", !is_protobuf);
      wd.setChecked("checkBoxTimestamp", use_embedded_timestamp_);

      // Protobuf-specific options
      wd.setVisible("boxProtobufOptions", is_protobuf);
      if (is_protobuf) {
        // Proto File tab
        wd.setFilePicker("buttonLoadProto", "Load .proto file", "Proto files (*.proto)", "Load .proto file");
        wd.setText("lineEditProtoFilePath", proto_file_path_);
        wd.setItems("comboMessageType", proto_message_types_);
        wd.setCurrentIndex("comboMessageType", messageTypeIndex());
        wd.setChecked("checkBoxProtoTimestamp", use_embedded_timestamp_);
        wd.setText("plainTextEditProtoContent", proto_file_content_);

        // Include Folders tab: file picker for add, list for display
        wd.setFilePicker("buttonAddFolder", "Add include Folder", "All files (*)", "Select file in include folder");
        wd.setListItems("listWidgetFolders", include_folders_);
      }
    }

    // Topic filter
    wd.setText("lineEditTopics", topic_filter_);

    wd.setOkEnabled(true);

    return wd.toJson();
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "radioConnect") {
      connect_mode_ = checked;
      return false;
    }
    if (widget_name == "radioBind") {
      connect_mode_ = !checked;
      return false;
    }
    if (widget_name == "checkBoxTimestamp") {
      use_embedded_timestamp_ = checked;
      return false;
    }
    if (widget_name == "checkBoxProtoTimestamp") {
      use_embedded_timestamp_ = checked;
      return false;
    }
    return false;
  }

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBox") {
      transport_ = indexToTransport(index);
      return false;
    }
    if (widget_name == "comboMessageType") {
      if (index >= 0 && index < static_cast<int>(proto_message_types_.size())) {
        proto_message_type_ = proto_message_types_[static_cast<size_t>(index)];
      }
      return false;
    }
    if (widget_name == "comboBoxProtocol") {
      const auto& encs = encodings_.empty() ? kKnownEncodings() : encodings_;
      if (index >= 0 && index < static_cast<int>(encs.size())) {
        encoding_ = encs[static_cast<size_t>(index)];
      }
      // Refrescar widget_data para actualizar visibilidad de opciones
      return true;
    }
    return false;
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditAddress") {
      address_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditPort") {
      auto val = std::atoi(std::string(text).c_str());
      if (val > 0 && val <= 65535) port_ = val;
      return false;
    }
    if (widget_name == "lineEditTopics") {
      topic_filter_ = std::string(text);
      return false;
    }
    return false;
  }

  bool onFileSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonLoadProto") {
      proto_file_path_ = std::string(path);
      // Read the file content for the text area and extract message types
      proto_file_content_.clear();
      proto_message_types_.clear();
      try {
        std::ifstream f(proto_file_path_);
        if (f) {
          proto_file_content_ = std::string(std::istreambuf_iterator<char>(f),
                                             std::istreambuf_iterator<char>());
          proto_message_types_ = extractMessageTypes(proto_file_content_);
        }
      } catch (...) {
      }
      // Seleccionar el primero si no hay tipo seleccionado aún
      if (proto_message_type_.empty() && !proto_message_types_.empty()) {
        proto_message_type_ = proto_message_types_.front();
      }
      return true;
    }
    if (widget_name == "buttonAddFolder") {
      // Extract the parent directory from the selected file path
      std::string folder = std::string(path);
      try {
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
          folder = parent.string();
        }
      } catch (...) {
      }
      // Avoid duplicates
      for (const auto& f : include_folders_) {
        if (f == folder) return false;
      }
      include_folders_.push_back(folder);
      // Refresh to update list
      return true;
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonRemoveFolder") {
      if (!selected_folder_.empty()) {
        auto it = std::find(include_folders_.begin(), include_folders_.end(), selected_folder_);
        if (it != include_folders_.end()) {
          include_folders_.erase(it);
          selected_folder_.clear();
          return true;
        }
      }
      return false;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name,
                          const std::vector<std::string>& selected) override {
    if (widget_name == "listWidgetFolders") {
      selected_folder_ = selected.empty() ? std::string{} : selected.front();
      return false;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = address_;
    cfg["port"] = port_;
    cfg["transport"] = transport_;
    cfg["mode"] = connect_mode_ ? "connect" : "bind";
    cfg["topics"] = topic_filter_;
    cfg["default_encoding"] = encoding_;
    // Parser-specific options.
    // available_encodings NO se persiste (lo inyecta el host en runtime).
    cfg["parser_config"]["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["parser_config"]["proto_file"] = proto_file_path_;
    cfg["parser_config"]["proto_message_type"] = proto_message_type_;
    cfg["parser_config"]["include_folders"] = include_folders_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;

    address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 9872);
    transport_ = cfg.value("transport", std::string("tcp://"));
    connect_mode_ = cfg.value("mode", std::string("connect")) == "connect";
    topic_filter_ = cfg.value("topics", std::string{});
    encoding_ = cfg.value("default_encoding", std::string("json"));

    if (cfg.contains("parser_config") && cfg["parser_config"].is_object()) {
      const auto& pc = cfg["parser_config"];
      use_embedded_timestamp_ = pc.value("use_embedded_timestamp", false);
      proto_file_path_ = pc.value("proto_file", std::string{});
      proto_message_type_ = pc.value("proto_message_type", std::string{});
      // Reconstruir contenido y tipos desde la ruta guardada
      proto_file_content_.clear();
      proto_message_types_.clear();
      if (!proto_file_path_.empty()) {
        try {
          std::ifstream f(proto_file_path_);
          if (f) {
            proto_file_content_ = std::string(std::istreambuf_iterator<char>(f),
                                               std::istreambuf_iterator<char>());
            proto_message_types_ = extractMessageTypes(proto_file_content_);
          }
        } catch (...) {
        }
      }
      include_folders_.clear();
      if (pc.contains("include_folders") && pc["include_folders"].is_array()) {
        for (const auto& f : pc["include_folders"]) {
          if (f.is_string()) include_folders_.push_back(f.get<std::string>());
        }
      }
    }

    // Available encodings injected by the host: merge with known list
    if (cfg.contains("available_encodings") && cfg["available_encodings"].is_array()) {
      encodings_ = kKnownEncodings();
      for (const auto& e : cfg["available_encodings"]) {
        if (!e.is_string()) continue;
        const auto& s = e.get<std::string>();
        bool already = false;
        for (const auto& k : encodings_) {
          if (k == s) {
            already = true;
            break;
          }
        }
        if (!already) encodings_.push_back(s);
      }
    }

    return true;
  }

 private:
  /// Extrae los nombres de tipo "message FooBar {" del contenido de un .proto
  static std::vector<std::string> extractMessageTypes(const std::string& content) {
    std::vector<std::string> types;
    std::regex re(R"(\bmessage\s+(\w+)\s*\{)");
    auto begin = std::sregex_iterator(content.begin(), content.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      types.push_back((*it)[1].str());
    }
    return types;
  }

  int messageTypeIndex() const {
    for (int i = 0; i < static_cast<int>(proto_message_types_.size()); ++i) {
      if (proto_message_types_[static_cast<size_t>(i)] == proto_message_type_) return i;
    }
    return 0;
  }

  /// Protocolos que tienen sección de opciones UI
  static bool hasOptions(const std::string& enc) {
    return enc == "json" || enc == "bson" || enc == "msgpack" || enc == "protobuf";
  }

  /// Lista completa de protocolos conocidos — siempre visible en el combo
  static std::vector<std::string> kKnownEncodings() {
    return {"json", "protobuf", "cdr", "influx", "bson", "cbor", "msgpack"};
  }

  static int transportToIndex(const std::string& t) {
    if (t == "ipc://") return 1;
    if (t == "pgm://") return 2;
    return 0;
  }

  static std::string indexToTransport(int idx) {
    switch (idx) {
      case 1: return "ipc://";
      case 2: return "pgm://";
      default: return "tcp://";
    }
  }

  static int encodingToIndex(const std::string& enc, const std::vector<std::string>& list) {
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
      if (list[static_cast<size_t>(i)] == enc) return i;
    }
    return 0;
  }

  std::string address_ = "localhost";
  int port_ = 9872;
  std::string transport_ = "tcp://";
  bool connect_mode_ = true;
  std::string topic_filter_;
  std::string encoding_ = "json";
  bool use_embedded_timestamp_ = false;
  std::string proto_file_path_;
  std::string proto_file_content_;
  std::string proto_message_type_;
  std::vector<std::string> proto_message_types_;
  std::vector<std::string> include_folders_;
  std::string selected_folder_;
  std::vector<std::string> encodings_;  // merged from host + kKnownEncodings
};

}  // namespace
