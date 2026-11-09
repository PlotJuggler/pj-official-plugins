#pragma once

// CanDialog: picks the QtSerialBus backend + interface, the bus/data bitrates, the timestamp source, the
// dictionaries (DBC or ARUS-style CSV) and the raw fallback of one CAN streamer source. Qt-free: the
// backend/interface lists come through the injected functions (can_bus_reader.hpp by default).

#include <functional>
#include <pj_base/expected.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "can_bus_reader.hpp"

namespace can_stream {

class CanDialog : public PJ::DialogPluginTyped {
 public:
  using ListBackends = std::function<std::vector<std::string>()>;
  using ListInterfaces = std::function<PJ::Expected<std::vector<std::string>>(const std::string&)>;

  explicit CanDialog(
      ListBackends list_backends = can_stream::availableBackends,
      ListInterfaces list_interfaces = can_stream::availableInterfaces);

  // --- Config state the source reads back ---
  BusSettings busSettings() const {
    return settings_;
  }
  /// Stored as given: a stream has no data file for relative paths to be relative to.
  const std::vector<std::string>& dictionaries() const {
    return dictionaries_;
  }
  bool rawUnassigned() const {
    return raw_unassigned_;
  }

  // --- Dialog protocol ---
  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  /// Every key is optional; a missing or wrongly-typed one keeps its default. False only for unparsable JSON.
  bool loadConfig(std::string_view config_json) override;
  std::string saveConfig() const override;
  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  bool onToggled(std::string_view widget_name, bool checked) override;
  bool onClicked(std::string_view widget_name) override;
  bool onFileSelected(std::string_view widget_name, std::string_view path) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

 private:
  void refreshBackends();
  void refreshInterfaces();

  ListBackends list_backends_;
  ListInterfaces list_interfaces_;

  BusSettings settings_;
  std::vector<std::string> dictionaries_;
  bool raw_unassigned_ = true;

  // What the combos show; backends_ also holds a saved backend not installed here, so it survives a save.
  bool lists_loaded_ = false;
  std::vector<std::string> backends_;
  std::vector<std::string> interfaces_;
  std::string interfaces_error_;
  std::string selected_dictionary_;
};

}  // namespace can_stream
