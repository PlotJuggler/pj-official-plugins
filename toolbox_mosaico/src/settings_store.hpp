// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <vector>

namespace mosaico {

// Thin, QSettings-like adapter over the host's `pj.settings.v1` service
// (PJ::sdk::SettingsView). Scalars are persisted as strings by the service;
// reads fall back to the supplied default when the key is absent, the store is
// unbound (optional service), or the host backend faults — preserving a
// non-throwing contract.
class SettingsStore {
 public:
  explicit SettingsStore(PJ::sdk::SettingsView view) : view_(view) {}

  std::string getString(const std::string& key, const std::string& def = "") const;
  void setString(const std::string& key, const std::string& value);

  std::vector<std::string> getStringList(const std::string& key) const;
  void setStringList(const std::string& key, const std::vector<std::string>& values);

  int getInt(const std::string& key, int def) const;
  void setInt(const std::string& key, int value);

  bool getBool(const std::string& key, bool def) const;
  void setBool(const std::string& key, bool value);

 private:
  PJ::sdk::SettingsView view_;
};

}  // namespace mosaico
