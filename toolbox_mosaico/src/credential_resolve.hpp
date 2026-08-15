// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Shared per-server credential resolution, hoisted from mosaico_dialog.cpp so
// the dialog's connect paths and the descriptor-import provider resolve
// credentials through ONE implementation.
//
// THREADING CONTRACT — MAIN THREAD ONLY. Every function here takes a
// PJ::sdk::SettingsView, and every SettingsView call is main-thread-only by
// the SDK ABI. Callers resolve on the main thread and hand worker/job threads
// an immutable BY-VALUE ServerCredentials copy: the dialog resolves inside
// its GUI-thread handlers; the provider's start_import resolves inside the
// (main-thread) start call — the job thread never touches the view.
#pragma once

#include <pj_base/sdk/plugin_data_api.hpp>  // PJ::sdk::SettingsView
#include <string>

#include "worker_types.h"

namespace mosaico {

/// The settings namespace for `uri`'s per-server entry
/// ("mosaico/server_cache/<normalizeServerKey(uri)>/").
[[nodiscard]] std::string credentialsSettingsPrefix(const std::string& uri);

/// Read the stored per-server credentials; absent keys leave defaults.
[[nodiscard]] ServerCredentials loadCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri);

/// Persist `creds` under the per-server prefix.
void saveCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri, const ServerCredentials& creds);

/// INTERACTIVE resolution (the panel's connect paths): stored per-server key,
/// falling back to the MOSAICO_API_KEY environment variable unguarded — the
/// user chose the server in the panel, so the env key applies to whatever
/// they typed (PJ3 automation parity).
[[nodiscard]] ServerCredentials resolveCredentials(PJ::sdk::SettingsView view, const std::string& uri);

/// Whether the MOSAICO_API_KEY env key may be released to `target_uri` on the
/// HEADLESS path: true iff `mosaico_url_env` (the MOSAICO_URL variable) is
/// non-empty and its origin equals the target's (strict, fail-closed). A
/// hostile layout naming a different server must never receive the env key.
/// Pure — exposed for the origin-guard test matrix.
[[nodiscard]] bool envKeyAllowedForTarget(const std::string& target_uri, const std::string& mosaico_url_env);

/// HEADLESS resolution (the descriptor-import job): stored per-server key
/// first, then the env key ONLY under envKeyAllowedForTarget's origin guard.
[[nodiscard]] ServerCredentials resolveHeadlessCredentials(PJ::sdk::SettingsView view, const std::string& uri);

}  // namespace mosaico
