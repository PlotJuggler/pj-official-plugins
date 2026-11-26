// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>

namespace assistant_agent {

// A plain-text listing of what is loaded (topics, fields, types) handed to the
// model up front so it does not have to ask. Carries no statistics: min/max
// would mean scanning every series. GUI thread only, like the host views it reads.
//
// Bounded by `budget_chars`: per topic it picks the cheapest rendering that fits
// (all fields with types, a partial field list, or a field count), falling back
// to topic names only. Whatever is left out is stated explicitly, because a model
// that takes an incomplete listing for the whole truth will tell the user a
// signal does not exist. It points at the MCP tools list_topics / describe_topic.
//
// Object topics get one line each, with their time range in display seconds when
// `playback` can convert it, raw seconds (and said so) otherwise.
[[nodiscard]] std::string catalogDigest(
    const PJ::sdk::ToolboxHostView& host, std::size_t budget_chars = 10000,
    const PJ::sdk::PlaybackHostView& playback = {});

}  // namespace assistant_agent
