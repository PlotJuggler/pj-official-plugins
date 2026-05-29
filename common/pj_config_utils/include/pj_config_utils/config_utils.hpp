// Shared JSON config-parsing helpers for plugin saveConfig()/loadConfig().
//
// Extracted from the ~18 copies of the same parse-then-check-discarded idiom
// scattered across the plugins. The helpers encode a single, documented
// two-tier policy for a *present but malformed* config string:
//
//   * Data sources (file/stream importers) treat malformed config as a HARD
//     error — a source that cannot read its own config (e.g. its filepath)
//     must not silently start with defaults. Use `parseStrict`.
//
//   * Parsers (message decoders) treat malformed config as RECOVERABLE — a bad
//     flag should not abort decoding of an otherwise-valid stream, so the
//     parser keeps its built-in defaults. The caller is told it was malformed
//     so it can emit a warning instead of failing silently. Use `parseLenient`.
//
// In BOTH tiers an *empty* config string is valid and yields an empty object
// ("no config — use defaults" is never an error). Field extraction
// (`cfg.value(key, default)`) stays in the plugin; only the parse + validation
// step is shared here.
#pragma once

#include <nlohmann/json.hpp>
#include <pj_base/expected.hpp>
#include <string_view>

namespace pj::config {

/// Strict parse for DATA SOURCES. Empty input → empty object (OK). A non-empty
/// but malformed config is a hard error: returns `unexpected("invalid <context>
/// JSON")` so the failure surfaces to the host.
[[nodiscard]] PJ::Expected<nlohmann::json> parseStrict(
    std::string_view config_json, std::string_view context = "config");

/// Lenient parse for PARSERS. Never fails: empty or malformed input → empty
/// object (the caller's `cfg.value(key, default)` calls then yield defaults).
/// When @p out_was_malformed is non-null it is set to true iff a non-empty
/// input failed to parse, so the caller can log a warning rather than fail.
[[nodiscard]] nlohmann::json parseLenient(std::string_view config_json, bool* out_was_malformed = nullptr);

}  // namespace pj::config
