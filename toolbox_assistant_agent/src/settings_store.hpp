// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <vector>

namespace assistant_agent {

// Thin, QSettings-like adapter over the host's `pj.settings.v1` service. Never throws: reads
// fall back to the default when the key is absent, the (optional) service is unbound, or the
// host backend faults; writers are then silent no-ops.
class SettingsStore {
 public:
  explicit SettingsStore(PJ::sdk::SettingsView view) : view_(view) {}

  std::string getString(const std::string& key, const std::string& def = "") const;
  void setString(const std::string& key, const std::string& value);

  // False for an absent key, an unbound store or a host backend fault: the caller cannot tell
  // them apart, and none may read as "present" (it gates scrubLegacyConversationKeys' remove()).
  bool contains(const std::string& key) const;
  void remove(const std::string& key);

  std::vector<std::string> getStringList(const std::string& key) const;
  void setStringList(const std::string& key, const std::vector<std::string>& values);

  int getInt(const std::string& key, int def) const;
  void setInt(const std::string& key, int value);

  bool getBool(const std::string& key, bool def) const;
  void setBool(const std::string& key, bool value);

 private:
  PJ::sdk::SettingsView view_;
};

}  // namespace assistant_agent
