// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <string_view>

namespace assistant_agent {

// Default character budget for the features block, and the lowest value the
// "assistant.features_budget_chars" setting is clamped to
// (resolveFeaturesBudgetChars, assistant_dialog.hpp). The minimum is above the
// length of the built-in reference (fallback_features.hpp, pinned by a
// static_assert in features_digest.cpp), which is sent whole and never
// trimmed: a smaller budget could not be honoured.
inline constexpr std::size_t kDefaultFeaturesBudgetChars = 20000;
inline constexpr std::size_t kMinFeaturesBudgetChars = 5000;

// Renders the host's pj.feature_catalog.v1 document (schema 1) as the plain
// "PlotJuggler features" block prefixed to the user's message. Sections, in
// order: built in, known limitations, plugins loaded, installed but not
// active, available in the Marketplace; then a registry line when the
// Marketplace list was not fetched, and a fixed sentence on where plugins come
// from.
//
// Follows the document's extensibility rules: unknown fields are ignored, a
// missing optional field is unknown (it is left out, never rendered as "no"),
// and a `schema` above 1 or a document that does not parse yields the built-in
// reference (fallback_features.hpp) with a one-line note saying why. Host
// strings are flattened to one line each, because the block must never
// contain a blank line (stripCatalogPrefix, session_text.hpp, ends it at the
// first one).
//
// Over `budget_chars`, Marketplace summaries go first, then Marketplace
// entries from the end, with a count of what was left out. Built-in features,
// limitations and plugins already installed are never dropped silently: if
// they alone exceed the budget the block is cut with "(list truncated)".
// Deterministic: plugin lists are sorted by name, so the same document always
// renders the same text (and the same dedup hash, harness_memory.hpp).
[[nodiscard]] std::string featuresDigest(std::string_view json, std::size_t budget_chars = kDefaultFeaturesBudgetChars);

// The block for one turn: the host's document when the service is bound and
// answers, the built-in reference otherwise (an unbound view is a host older
// than SDK 0.38.0, which is the floor case manifest.json's floor_test covers).
// GUI thread only, like every host view.
[[nodiscard]] std::string featuresBlock(const PJ::sdk::FeatureCatalogView& view, std::size_t budget_chars);

}  // namespace assistant_agent
