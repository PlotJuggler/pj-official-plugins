#pragma once

// Per-interface dictionary assignment (a .dbc or an ARUS-style .csv, translated in-memory to DBC text) plus a
// preview of the file: format, per-interface frame/id counts and inferred time mode.

#include <cstdint>
#include <map>
#include <optional>
#include <pj_can_dbc/j1939.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <vector>

#include "candump_parser.hpp"

namespace candump_detail {

class CandumpDialog : public PJ::DialogPluginTyped {
 public:
  /// Runs the bounded prescan that fills the summary and interface table.
  void setFilePath(const std::string& filepath);

  /// The picker yields one dictionary (dbc or csv) per interface; a hand-edited saved config may list more.
  void setInterfaceDicts(std::map<std::string, std::vector<std::string>> iface_dicts);

  // --- Config state the source reads back ---
  const std::string& filePath() const {
    return filepath_;
  }
  // Resolved paths to open (relative entries already joined to the current data
  // file's directory); the saved config stores the portable form instead.
  const std::map<std::string, std::vector<std::string>>& interfaceDicts() const {
    return iface_dicts_;
  }
  /// Interfaces whose frames are J1939 or NMEA 2000 (see pj_can_dbc::CanProtocol); an interface
  /// missing here is raw CAN.
  const std::map<std::string, pj_can_dbc::CanProtocol>& interfaceProtocols() const {
    return iface_protocols_;
  }
  bool rawUnassigned() const {
    return raw_unassigned_;
  }
  /// nullopt = auto-detect (the source falls back to its own prescan's
  /// TimeMode); otherwise the user's forced choice.
  std::optional<TimeMode> timeModeOverride() const;

  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;
  /// Like loadConfig() but defers the (possibly expensive) prescan to the next widget_data(), for
  /// CandumpSource::loadConfig()'s headless restore where nobody reads the summary.
  bool loadConfigDeferringScan(std::string_view config_json);
  bool onFileSelected(std::string_view widget_name, std::string_view path) override;
  bool onClicked(std::string_view widget_name) override;
  bool onToggled(std::string_view widget_name, bool checked) override;
  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  /// Exposed for tests.
  static bool isValidInterfaceKey(std::string_view key);

 private:
  void scanFile();
  bool applyConfigJson(std::string_view config_json, bool scan_now);
  std::string currentInterface() const;
  pj_can_dbc::CanProtocol protocolOf(const std::string& iface) const;

  static constexpr std::uint64_t kTailBytes = 65536;

  std::string filepath_;
  std::string summary_;
  std::vector<std::string> interface_names_;              // interface per table row, in prescan order
  std::vector<std::vector<std::string>> interface_rows_;  // table rows, parallel to interface_names_
  // "absolute" / "relative" / "delta", or empty if the prescan saw no numeric timestamp.
  std::string detected_time_mode_text_;
  bool needs_scan_ = false;  // true between loadConfigDeferringScan() and the next widget_data()
  int selected_interface_index_ = 0;

  std::map<std::string, std::vector<std::string>> iface_dicts_;
  std::map<std::string, pj_can_dbc::CanProtocol> iface_protocols_;
  bool raw_unassigned_ = true;
  int time_mode_override_ = 0;
};

}  // namespace candump_detail
