#pragma once

// CandumpDialog: per-interface dictionary assignment (a .dbc or an
// ARUS-style .csv, translated in-memory to DBC text) plus a preview of what
// the file contains -- format, per-interface frame/id counts, and the
// inferred time mode. Modeled on blf_detail::BlfDialog (per-channel DBC
// picker replacing the whole entry, saved config surviving a re-scan that
// didn't see a previously-configured interface).

#include <cstdint>
#include <map>
#include <optional>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <vector>

#include "candump_parser.hpp"

namespace candump_detail {

class CandumpDialog : public PJ::DialogPluginTyped {
 public:
  /// Points the dialog at a file; runs the bounded prescan for the summary
  /// and interface table.
  void setFilePath(const std::string& filepath);

  /// Per-interface dictionary paths configured on the source (dbc or csv,
  /// one entry per interface from the single file picker; more than one via
  /// a hand-edited saved config -- same convention as BlfDialog::setChannelDbcs).
  void setInterfaceDicts(std::map<std::string, std::vector<std::string>> iface_dicts);

  // --- Config state the source reads back ---
  const std::string& filePath() const {
    return filepath_;
  }
  const std::map<std::string, std::vector<std::string>>& interfaceDicts() const {
    return iface_dicts_;
  }
  bool rawUnassigned() const {
    return raw_unassigned_;
  }
  /// nullopt = auto-detect (the source falls back to its own prescan's
  /// TimeMode); otherwise the user's forced choice.
  std::optional<TimeMode> timeModeOverride() const;

  // --- Dialog protocol ---
  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;
  /// Same parsing as loadConfig(), but does not run the (possibly expensive)
  /// file prescan immediately -- for CandumpSource::loadConfig()'s
  /// config-only/headless restore, where nobody reads the scan's summary/
  /// interface-table output. The scan instead runs lazily on the next
  /// widget_data() call (i.e. only once the dialog is actually rendered),
  /// so the UI-visible result is identical either way.
  bool loadConfigDeferringScan(std::string_view config_json);
  bool onFileSelected(std::string_view widget_name, std::string_view path) override;
  bool onClicked(std::string_view widget_name) override;
  bool onToggled(std::string_view widget_name, bool checked) override;
  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  /// Exposed for tests: true iff `key` is an acceptable interface name for
  /// the config map (non-empty, <= 15 chars, no '/').
  static bool isValidInterfaceKey(std::string_view key);

 private:
  void scanFile();
  bool applyConfigJson(std::string_view config_json, bool scan_now);
  std::string currentInterface() const;

  static constexpr std::uint64_t kTailBytes = 65536;

  std::string filepath_;
  std::string summary_;
  std::vector<std::string> interface_names_;              // interface per table row, in prescan order
  std::vector<std::vector<std::string>> interface_rows_;  // table rows, parallel to interface_names_
  // "absolute" / "relative" / "delta", or empty if the prescan saw no numeric timestamp at all.
  // Feeds the "Automatic (<detected>)" comboTimeMode item text.
  std::string detected_time_mode_text_;
  bool needs_scan_ = false;  // true between loadConfigDeferringScan() and the next widget_data()
  int selected_interface_index_ = 0;

  std::map<std::string, std::vector<std::string>> iface_dicts_;
  bool raw_unassigned_ = true;
  int time_mode_override_ = 0;
};

}  // namespace candump_detail
