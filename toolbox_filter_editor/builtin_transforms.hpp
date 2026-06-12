#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The 12 builtin Filter Transform strategies live in the SDK
// (plotjuggler_sdk/pj_plugins/filter_protocol/include/pj_plugins/sdk/builtin_transforms.hpp).
// This shim re-exports them so the plugin and the host can both consume the
// same source.

#include "pj_plugins/sdk/builtin_transforms.hpp"
