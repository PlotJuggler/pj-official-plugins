// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "settings_store.hpp"

namespace mosaico {

// On a backend-fault Expected (!has_value()) every getter returns the default
// and every setter is a silent no-op, preserving the non-throwing contract.

std::string SettingsStore::getString(const std::string& key, const std::string& def) const {
  if (auto v = view_.value(key)) {
    return v->toString(def);
  }
  return def;
}

void SettingsStore::setString(const std::string& key, const std::string& value) {
  (void)view_.setValue(key, value);
}

std::vector<std::string> SettingsStore::getStringList(const std::string& key) const {
  if (auto v = view_.valueStringList(key)) {
    return *v;
  }
  return {};
}

void SettingsStore::setStringList(const std::string& key, const std::vector<std::string>& values) {
  (void)view_.setValue(key, values);
}

int SettingsStore::getInt(const std::string& key, int def) const {
  if (auto v = view_.value(key)) {
    return static_cast<int>(v->toInt(def));
  }
  return def;
}

void SettingsStore::setInt(const std::string& key, int value) {
  (void)view_.setValue(key, value);
}

bool SettingsStore::getBool(const std::string& key, bool def) const {
  if (auto v = view_.value(key)) {
    return v->toBool(def);
  }
  return def;
}

void SettingsStore::setBool(const std::string& key, bool value) {
  (void)view_.setValue(key, value);
}

}  // namespace mosaico
