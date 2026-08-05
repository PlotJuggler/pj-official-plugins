// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>  // sdk::ToolboxHostView, sdk::FieldHandle
#include <pj_plugins/sdk/widget_data.hpp>   // PJ::ChartPoint
#include <string>
#include <unordered_map>
#include <vector>

#include "toolbox_preview/adaptive_poll.hpp"

namespace toolbox_preview {

/// Rebase ns timestamps to seconds-from-start and downsample to <= `max_points` by
/// striding. Shared by the background source curve and any derived-series overlay.
[[nodiscard]] std::vector<PJ::ChartPoint> downsampleToChart(
    const std::vector<double>& timestamps_ns, const std::vector<double>& values, std::size_t max_points = 2000);

/// Source-series cache for a generator toolbox preview. Enumerates the float64 series
/// the toolbox host exposes, caches their samples for the background curve, and live-
/// refreshes the *selected* source while streaming so the preview follows the window.
/// UI-free and generator-agnostic — the plugin owns the dialog and the output rendering.
///
/// Lifetime: every method takes the plugin's long-lived `ToolboxHostView` (from
/// `ToolboxPluginBase::toolboxHost()`); the materialized series borrow from it for the
/// duration of the call only.
class SeriesCatalog {
 public:
  /// Re-read every float64 series only when the catalog STRUCTURE changed — an FNV
  /// signature over data-source ids + per-topic field counts. A cheap no-op otherwise,
  /// so it is safe to call every panel tick. Returns true when a refresh happened (the
  /// caller then refills its source list and marks the preview dirty).
  bool refreshStructureIfChanged(const PJ::sdk::ToolboxHostView& host);

  /// Force a full re-read regardless of the signature (call before submitting a
  /// committed generator so the input list is current). Returns true if the host was
  /// readable.
  bool refresh(const PJ::sdk::ToolboxHostView& host);

  /// Throttled live re-read of ONLY `selected` (cheap for a bounded streaming window),
  /// refreshing its cached samples so the curve + `t0` track the stream. Returns true
  /// when new samples were read; an unchanged/paused source returns false and the
  /// preview stays frozen.
  bool refreshSelectedSourceData(const PJ::sdk::ToolboxHostView& host, const std::string& selected);

  [[nodiscard]] const std::vector<std::string>& names() const {
    return series_names_;
  }
  [[nodiscard]] bool has(const std::string& name) const {
    return series_map_.find(name) != series_map_.end();
  }
  /// Monotonic counter that increments whenever any cached series DATA changes (a full
  /// `refresh` or a live `refreshSelectedSourceData` that read new samples). A consumer
  /// caches a derived view and re-derives only when this value moves — so per-tick work
  /// scales with data change, not the panel tick rate. Structure-only no-ops don't bump it.
  [[nodiscard]] std::uint64_t dataRevision() const {
    return data_revision_;
  }
  /// Front timestamp (ns, as double) of `name` — the preview X origin — or 0 if the
  /// series is absent/empty.
  [[nodiscard]] double t0(const std::string& name) const;
  /// Downsampled, t0-rebased points for the background curve (empty if absent).
  [[nodiscard]] std::vector<PJ::ChartPoint> points(const std::string& name, std::size_t max_points = 2000) const;

 private:
  struct SeriesData {
    std::vector<double> timestamps;  ///< nanoseconds
    std::vector<double> values;
  };

  // Materialize a host float64 series into our double cache: convert int64 ns timestamps to
  // double and bulk-copy the (already double) values. Shared by the full and single-source
  // refresh so the conversion lives in exactly one place.
  template <typename TsSpan>
  static SeriesData buildSeriesData(const TsSpan& ts, const double* values, std::size_t count) {
    SeriesData sa;
    sa.timestamps.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
      sa.timestamps[i] = static_cast<double>(ts[i]);  // int64 ns -> double
    }
    sa.values.assign(values, values + count);  // already double: bulk copy
    return sa;
  }
  std::unordered_map<std::string, SeriesData> series_map_;
  std::unordered_map<std::string, PJ::sdk::FieldHandle> field_handles_;  ///< name → handle, for the live re-read
  std::vector<std::string> series_names_;
  std::uint64_t last_catalog_sig_ = UINT64_MAX;  ///< forces the first refresh; guards live ticks
  std::uint64_t data_revision_ = 0;              ///< bumps when cached series DATA changes (see dataRevision)

  // --- streaming live-refresh state (selected source only) ---
  std::string last_source_name_;
  std::size_t last_source_count_ = 0;
  double last_source_last_ts_ = 0.0;
  // Read the selected source every tick while it MOVES (smooth streaming), backing off only
  // once it is static — so a huge non-streaming series isn't re-materialized every tick. The
  // cadence rule lives in AdaptivePoll; this class just reads.
  AdaptivePoll source_poll_;
};

}  // namespace toolbox_preview
