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

/// A derived-series "after" curve (transform) and/or a marker overlay (markers); a backend fills
/// whichever it produces.
struct PreviewOverlay {
  std::vector<PJ::ChartSeries> series;
  std::vector<PJ::ChartMarker> markers;
};

/// What a concrete toolbox supplies to the shared preview: how to submit its ephemeral generator
/// and how to turn the host-published output into a chart overlay. The ONLY generator-specific
/// part of the preview.
class PreviewBackend {
 public:
  virtual ~PreviewBackend() = default;

  /// Submits/replaces the ephemeral generator running `code`. The backend supplies its own input
  /// series (it owns the catalog). Returns the resolved output topic to read back, an EMPTY
  /// string for "not available right now" (e.g. the host service is unbound: no error surfaced,
  /// retried next refresh), or an error message for a compile/runtime rule failure.
  [[nodiscard]] virtual PJ::Expected<std::string> submitPreview(const std::string& code) = 0;

  /// Renders the latest output of `topic` as an overlay, rebased to `t0_ns`.
  [[nodiscard]] virtual PreviewOverlay readPreviewOverlay(const std::string& topic, double t0_ns) = 0;

  /// On dialog/toolbox teardown.
  virtual void removePreview() = 0;
};

/// The change-gated ephemeral preview shared by every generator toolbox.
///
/// CONTRACT: a generator submitted to the host is the host's to MAINTAIN. The ephemeral generator
/// is submitted ONCE and the HOST re-runs it on every matching commit (whole-series for markers,
/// `MarkerService::recomputeForChangedInputs`; incrementally for transforms,
/// `DataProcessorService::advanceOnCommit`), keeping its output topic fresh until removed. So it
/// is re-submitted ONLY on an intent change (rule / source / catalog structure).
///
/// Reading the output back is ALSO change-gated: the host only re-runs the generator on a commit,
/// and a commit moves the input series (`SeriesCatalog::dataRevision`). Between revision bumps the
/// output cannot have changed, so a CACHED overlay is returned instead of re-reading and
/// re-deserializing it (a large marker set would otherwise be converted on every 20 Hz tick).
///
/// Both submit and read-back therefore scale with DATA CHANGE, not with the UI tick rate.
class EphemeralPreview {
 public:
  /// Forces a re-submit on the next refresh(), e.g. after a catalog structure change leaves the
  /// generator's stored input list stale.
  void markDirty() {
    dirty_ = true;
  }

  /// The current overlay for (`code`, `source`). `t0_ns` is the preview X origin
  /// (`SeriesCatalog::t0`); `data_revision` is `SeriesCatalog::dataRevision()`: the read-back is
  /// skipped (cached overlay returned) while it is unchanged and no re-submit happened. On a rule
  /// error, sets `*error` (when non-null) and returns an empty overlay, leaving the
  /// last-submitted state untouched so the next refresh re-runs and re-surfaces it. Empty `code`
  /// or no successful run yet gives an empty overlay.
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

  // Failure latch: a rule that failed with this exact code at this data revision fails
  // identically every tick, so the cached error is re-surfaced instead of re-running the
  // expensive submit. submitPreview takes only `code`, so the source is not part of the latch.
  // Cleared on success; a new data revision retries once (a runtime error may depend on data).
  std::string last_failed_code_;
  std::string last_error_;
  std::uint64_t last_failed_revision_ = UINT64_MAX;
};

}  // namespace toolbox_preview
