// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>

namespace assistant_agent {

// A plain-text listing of what is currently loaded — topics, their fields and
// types — for handing to the model up front so it does not have to ask.
//
// This is metadata the host already holds, so building it is free; it
// deliberately carries no statistics (min/max would mean scanning every series,
// which just moves the cost rather than removing it). GUI thread only, like the
// host views it reads.
//
// Bounded by `budget_chars`. Per topic this picks the cheapest rendering that
// still fits — every field with its type, a partial field list, or just a
// field count — before falling back to the old two-step degradation (full
// tree, then topic names only) for a budget too tight even for bare counts.
// Whenever anything is left out the text says so explicitly — a model that
// believes an incomplete listing is the whole truth will confidently tell the
// user a signal does not exist. The tool names it points at (list_topics,
// describe_topic) are the ones PlotJuggler's MCP server serves.
//
// Object topics get one line each, with their time range in display seconds
// when `playback` can convert it, raw seconds (and said so) otherwise.
[[nodiscard]] std::string catalogDigest(
    const PJ::sdk::ToolboxHostView& host, std::size_t budget_chars = 10000,
    const PJ::sdk::PlaybackHostView& playback = {});

}  // namespace assistant_agent
