// Copyright 2026 PlotJuggler Chinese localization contributors
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdlib>
#include <cstring>

// The host sets this once at startup, before loading plugins or starting workers.
// UI preference only; never used for protocol/configuration/data translation.
namespace pj::localization {
enum class Language { English, SimplifiedChinese, TraditionalChinese };

// Take an owned, immutable decision once per plugin image. Never retain a
// pointer into the environment or race worker threads with preference writes.
// Only the host's canonical IDs are accepted; missing/unknown values use English.
inline Language language() {
  static const Language selected = [] {
#ifdef _WIN32
    char value[6] = {};
    std::size_t size = 0;
    // Query first so an unknown, oversized value never reaches the CRT's
    // small-buffer error path. The canonical IDs have five ASCII characters.
    if (getenv_s(&size, nullptr, 0, "PLOTJUGGLER_UI_LANGUAGE") != 0 || size != sizeof(value) ||
        getenv_s(&size, value, sizeof(value), "PLOTJUGGLER_UI_LANGUAGE") != 0) {
      return Language::English;
    }
#else
    const char* value = std::getenv("PLOTJUGGLER_UI_LANGUAGE");
    if (value == nullptr) return Language::English;
#endif
    if (std::strcmp(value, "zh_CN") == 0) return Language::SimplifiedChinese;
    if (std::strcmp(value, "zh_TW") == 0) return Language::TraditionalChinese;
    return Language::English;
  }();
  return selected;
}

// Use only for display grammar shared by both Chinese scripts.
inline bool isChinese() {
  return language() != Language::English;
}

// All arguments are fixed display prose. User data and canonical identifiers
// must be appended by the caller without translation or character conversion.
inline const char* text(const char* english, const char* simplified, const char* traditional) {
  switch (language()) {
    case Language::SimplifiedChinese:
      return simplified;
    case Language::TraditionalChinese:
      return traditional;
    case Language::English:
      return english;
  }
  return english;
}
}  // namespace pj::localization
