// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <string_view>

#include "descriptor_import/source_descriptor.hpp"

namespace mosaico {

/// Top-level QSettings group read by PJ4's Load-Layout dialog. The suffix is
/// base64url(identity UTF-8 bytes), without '=' padding, so '/' in a descriptor
/// identity can never become a QSettings group separator.
[[nodiscard]] std::string sourcePresentationSettingsGroup(std::string_view identity);

/// Record the human presentation for one descriptor identity through the
/// host-provided pj.settings.v1 view. Main-thread only. Values are capped to
/// the host dialog's 200-character limit and unchanged values are not written.
void recordSourcePresentation(
    PJ::sdk::SettingsView settings, std::string_view identity, const SourceDescriptor& descriptor);

}  // namespace mosaico
