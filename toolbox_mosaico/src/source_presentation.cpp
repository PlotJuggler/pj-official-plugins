// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "source_presentation.hpp"

#include <cstddef>
#include <cstdint>
#include <pj_base/sdk/descriptor_import/origin.hpp>
#include <string>
#include <string_view>

namespace mosaico {

namespace {

constexpr std::size_t kMaxPresentationChars = 200;
constexpr std::string_view kSettingsPrefix = "source_presentation/v1/";
constexpr char kBase64UrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

const PJ::sdk::descriptor_import::OriginPolicy& grpcOriginPolicy() {
  static const PJ::sdk::descriptor_import::OriginPolicy policy{{"grpc", "grpc+tls"}, {}};
  return policy;
}

std::string base64UrlUnpadded(std::string_view bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);

  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const std::uint32_t value = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                                static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
    out.push_back(kBase64UrlAlphabet[(value >> 18) & 0x3f]);
    out.push_back(kBase64UrlAlphabet[(value >> 12) & 0x3f]);
    out.push_back(kBase64UrlAlphabet[(value >> 6) & 0x3f]);
    out.push_back(kBase64UrlAlphabet[value & 0x3f]);
  }

  const std::size_t remaining = bytes.size() - i;
  if (remaining == 1) {
    const std::uint32_t value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
    out.push_back(kBase64UrlAlphabet[(value >> 18) & 0x3f]);
    out.push_back(kBase64UrlAlphabet[(value >> 12) & 0x3f]);
  } else if (remaining == 2) {
    const std::uint32_t value = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
    out.push_back(kBase64UrlAlphabet[(value >> 18) & 0x3f]);
    out.push_back(kBase64UrlAlphabet[(value >> 12) & 0x3f]);
    out.push_back(kBase64UrlAlphabet[(value >> 6) & 0x3f]);
  }
  return out;
}

// QSettings stores QString values and the host applies QString::left(200).
// Bound UTF-8 by the corresponding UTF-16 code-unit count without splitting a
// multi-byte code point (notably, an astral code point consumes two QChars).
std::string capPresentation(std::string_view text) {
  std::size_t bytes = 0;
  std::size_t utf16_units = 0;
  while (bytes < text.size()) {
    const auto lead = static_cast<unsigned char>(text[bytes]);
    std::size_t width = 1;
    std::size_t units = 1;
    if (lead >= 0xc2 && lead <= 0xdf) {
      width = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      width = 3;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      width = 4;
      units = 2;
    }

    bool complete = bytes + width <= text.size();
    for (std::size_t offset = 1; complete && offset < width; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[bytes + offset]);
      complete = (continuation & 0xc0) == 0x80;
    }
    if (!complete) {
      width = 1;
      units = 1;
    }
    if (utf16_units + units > kMaxPresentationChars) {
      break;
    }
    bytes += width;
    utf16_units += units;
  }
  return std::string(text.substr(0, bytes));
}

void setIfChanged(PJ::sdk::SettingsView settings, const std::string& key, const std::string& value) {
  const auto current = settings.value(key);
  // A backend read fault cannot establish equality, so attempt the write; all
  // presentation persistence is best-effort and never affects the Download.
  if (!current || current->toString() != value) {
    (void)settings.setValue(key, value);
  }
}

}  // namespace

std::string sourcePresentationSettingsGroup(std::string_view identity) {
  return std::string(kSettingsPrefix) + base64UrlUnpadded(identity);
}

void recordSourcePresentation(
    PJ::sdk::SettingsView settings_view, std::string_view identity, const SourceDescriptor& descriptor) {
  if (identity.empty()) {
    return;
  }

  const std::string group = sourcePresentationSettingsGroup(identity);
  const std::string display_name =
      capPresentation(descriptor.display_name.empty() ? descriptor.sequence : descriptor.display_name);

  setIfChanged(settings_view, group + "/display_name", display_name);
  if (const auto parsed_origin = PJ::sdk::descriptor_import::parseOrigin(descriptor.server_uri, grpcOriginPolicy())) {
    const std::string origin =
        capPresentation(parsed_origin->host + ":" + std::to_string(static_cast<unsigned int>(parsed_origin->port)));
    setIfChanged(settings_view, group + "/origin", origin);
  }
}

}  // namespace mosaico
