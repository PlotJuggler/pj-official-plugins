// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <pj_base/expected.hpp>            // PJ::Expected
#include <pj_plugins/sdk/widget_data.hpp>  // PJ::ChartSeries, PJ::ChartMarker
#include <string>
#include <string_view>
#include <vector>

namespace toolbox_preview {

/// The rendered result of a generator preview: a derived-series "after" curve
/// (transform) and/or a marker overlay (markers). A backend fills whichever it produces.
struct PreviewOverlay {
  std::vector<PJ::ChartSeries> series;
  std::vector<PJ::ChartMarker> markers;
};

/// What a concrete toolbox supplies to the shared preview: how to submit its ephemeral
/// generator and how to turn the host-published output into a chart overlay. This is the
/// ONLY generator-specific part of the preview — exactly "what it generates and where".
class PreviewBackend {
 public:
  virtual ~PreviewBackend() = default;

  /// Submit/replace the ephemeral generator running `code`. The backend supplies its own
  /// input series (it owns the catalog), so the generic engine doesn't carry an input list.
  /// Returns the resolved output topic to read back, an EMPTY string for "not available right
  /// now" (e.g. the host service is unbound — no error surfaced, retried next refresh), or an
  /// error message for a compile/runtime rule failure.
  [[nodiscard]] virtual PJ::Expected<std::string> submitPreview(const std::string& code) = 0;

  /// Read the latest output of `topic` and render it as an overlay, rebased to `t0_ns`.
  [[nodiscard]] virtual PreviewOverlay readPreviewOverlay(const std::string& topic, double t0_ns) = 0;

  /// Tear down the ephemeral generator (on dialog/toolbox teardown).
  virtual void removePreview() = 0;
};

/// The change-gated ephemeral preview shared by every generator toolbox.
///
/// CONTRACT (the architectural invariant this relies on): a generator submitted to the host is
/// the host's to MAINTAIN. So we submit the ephemeral generator ONCE and the HOST re-runs it on
/// every matching commit — whole-series for markers (`MarkerService::recomputeForChangedInputs`),
/// incrementally for transforms (`DataProcessorService::advanceOnCommit`) — keeping its output
/// topic fresh until removed. We therefore re-submit ONLY on an intent change (rule / source /
/// catalog structure).
///
/// Reading the output back is ALSO change-gated: the host only re-runs the generator on a commit,
/// and a commit moves the input series (its `SeriesCatalog::dataRevision`). So between revision
/// bumps the output cannot have changed, and we return a CACHED overlay instead of re-reading and
/// re-deserializing it. Without this, a large marker set was deserialized + converted on every
/// 20 Hz tick (the read-back side silently violated the contract the submit side honored).
///
/// Net: BOTH submit and read-back scale with DATA CHANGE (commits), not with the UI tick rate —
/// the deliberate opposite of re-creating the generator every frame, which would couple cost to
/// the refresh rate. UI-free.
class EphemeralPreview {
 public:
  /// Force a re-submit on the next refresh() — e.g. the catalog structure changed, so the
  /// generator's stored input list may be stale.
  void markDirty() {
    dirty_ = true;
  }

  /// Produce the current overlay for (`code`, `source`). `t0_ns` = the preview X origin (from
  /// `SeriesCatalog::t0`); `data_revision` = `SeriesCatalog::dataRevision()` — the read-back is
  /// skipped (cached overlay returned) while it is unchanged and no re-submit happened. On a
  /// rule error, sets `*error` (when non-null) and returns an empty overlay, leaving the
  /// last-submitted state untouched so the next refresh re-runs and re-surfaces it. Empty
  /// `code` or no successful run yet → empty.
  [[nodiscard]] PreviewOverlay refresh(
      PreviewBackend& backend, const std::string& code, const std::string& source, double t0_ns,
      std::uint64_t data_revision, std::string* error);

 private:
  bool dirty_ = true;  ///< forces the first submit
  std::string topic_;  ///< resolved output topic, read back between re-runs
  std::string last_code_;
  std::string last_source_;
  std::uint64_t last_read_revision_ = UINT64_MAX;  ///< data revision at last read-back; sentinel forces first read
  PreviewOverlay cached_overlay_;                  ///< returned between read-backs (see CONTRACT)

  // Failure latch: a rule that just failed with this exact code at this data
  // revision fails identically every tick, so re-surface the cached error instead
  // of re-running the expensive submit. (submitPreview takes only `code`, so the
  // source never affects the outcome — it is not part of the latch.) Cleared on
  // success; a new data revision retries once (a runtime error may depend on data).
  std::string last_failed_code_;
  std::string last_error_;
  std::uint64_t last_failed_revision_ = UINT64_MAX;
};

}  // namespace toolbox_preview
