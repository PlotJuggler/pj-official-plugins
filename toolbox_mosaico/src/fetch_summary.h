// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <string>

namespace mosaico {

// Decision produced once a fetch batch finishes (allFetchesComplete). Pure
// data so the close/import/error-summary policy is unit-testable without the
// Qt event loop or a live MosaicoClient.
struct FetchSummary {
  std::string status_text;     // what the status label shows
  std::string error_summary;   // deduped, newline-joined ("[3x] no data\ntimeout")
  bool should_close = false;   // close the panel (PJ3: after a successful batch)
  bool should_import = false;  // flush buffered writer chunks + refresh catalog
};

// Collapse identical error messages into "[Nx] msg" lines (PJ3
// showCopyableWarning dedup). Stable ordering follows the map's ordering.
inline std::string summarizeErrors(const std::map<std::string, int, std::less<>>& error_counts) {
  std::string out;
  for (const auto& [msg, count] : error_counts) {
    if (!out.empty()) {
      out += "\n";
    }
    if (count > 1) {
      out += "[" + std::to_string(count) + "x] ";
    }
    out += msg;
  }
  return out;
}

// PJ3 parity completion policy:
//   * cancelling           → "Download cancelled", stay open, no import
//   * any topic imported   → import + close (partial success still imports)
//   * nothing imported     → stay open showing the (deduped) errors
inline FetchSummary buildFetchSummary(
    int fetch_total, int fetch_done, int fetch_failed, bool imported_any, bool cancelling,
    const std::map<std::string, int, std::less<>>& error_counts) {
  (void)fetch_done;  // reserved for future "N/M completed" detail
  FetchSummary s;
  s.error_summary = summarizeErrors(error_counts);
  const int imported = fetch_total - fetch_failed;
  if (cancelling) {
    s.status_text = "Download cancelled";
    return s;
  }
  if (imported_any) {
    s.status_text = "Imported " + std::to_string(imported) + "/" + std::to_string(fetch_total) + " topics";
    s.should_close = true;
    s.should_import = true;
    return s;
  }
  s.status_text = "Fetch failed: " + s.error_summary;
  return s;
}

}  // namespace mosaico
