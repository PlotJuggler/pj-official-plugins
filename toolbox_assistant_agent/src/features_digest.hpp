// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <string_view>

namespace assistant_agent {

// Default character budget for the features block, and the floor the
// "assistant.features_budget_chars" setting is clamped to (resolveFeaturesBudgetChars).
// The floor exceeds the built-in reference's length (static_assert in
// features_digest.cpp): that reference is sent whole, so a smaller budget could not
// be honoured.
inline constexpr std::size_t kDefaultFeaturesBudgetChars = 20000;
inline constexpr std::size_t kMinFeaturesBudgetChars = 5000;

// Renders the host's pj.feature_catalog.v1 document (schema 1) as the plain
// "PlotJuggler features" block prefixed to the user's message: built in, known
// limitations, plugins loaded, installed but not active, available in the
// Marketplace, then a registry line if the Marketplace list was not fetched.
//
// Unknown fields are ignored and a missing optional field is left out, never
// rendered as "no". A `schema` above 1 or an unparsable document yields the built-in
// reference (fallback_features.hpp) with a one-line note. Host strings are flattened
// to one line: the block must never contain a blank line (stripCatalogPrefix ends it
// at the first one).
//
// Over `budget_chars`, Marketplace summaries go first, then Marketplace entries from
// the end, with a count of what was left out. Built-in features, limitations and
// installed plugins are never dropped silently; if they alone exceed the budget the
// block is cut with "(list truncated)". Deterministic (lists sorted by name), so the
// same document gives the same text and the same dedup hash (harness_memory.hpp).
[[nodiscard]] std::string featuresDigest(std::string_view json, std::size_t budget_chars = kDefaultFeaturesBudgetChars);

// The block for one turn: the host's document, or the built-in reference when the
// optional service is unbound (no note) or its document cannot be read (with a note).
// GUI thread only.
[[nodiscard]] std::string featuresBlock(const PJ::sdk::FeatureCatalogView& view, std::size_t budget_chars);

}  // namespace assistant_agent
