// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>  // sdk::ToolboxHostView, sdk::FieldHandle
#include <pj_plugins/sdk/widget_data.hpp>   // PJ::ChartPoint
#include <string>
#include <unordered_map>
#include <vector>

#include "toolbox_preview/adaptive_poll.hpp"
#include "toolbox_preview/series_types.hpp"  // isPlottableType, decodeSeriesAsDouble

namespace toolbox_preview {

/// Rebases ns timestamps to seconds-from-start and downsamples to <= `max_points` by striding.
/// Shared by the background source curve and any derived-series overlay.
[[nodiscard]] std::vector<PJ::ChartPoint> downsampleToChart(
    const std::vector<double>& timestamps_ns, const std::vector<double>& values, std::size_t max_points = 2000);

/// Source-series cache for a generator toolbox preview. Enumerates the series the toolbox host
/// exposes that can actually BE a source (see isPlottableType), caches their samples for the
/// background curve, and live-refreshes the *selected* source while streaming so the preview
/// follows the window. UI-free and generator-agnostic.
///
/// Invariant: `names()` is exactly the set of series the host accepts as a generator input.
/// Offering more (e.g. text fields) hands the user a choice that fails downstream with an opaque
/// Lua error. That set is fixed by `isPlottableType`, i.e. it is POLICY: a second consumer
/// wanting a different set (toolbox_fft deliberately excludes bool) should make the predicate
/// injectable rather than fork this class.
///
/// Lifetime: every method takes the plugin's long-lived `ToolboxHostView` (from
/// `ToolboxPluginBase::toolboxHost()`); the materialized series borrow from it for the duration
/// of the call only.
class SeriesCatalog {
 public:
  /// Re-reads every usable series only when the catalog STRUCTURE changed: an FNV signature over
  /// data-source ids, per-topic field counts, and each field's handle + TYPE. A cheap no-op
  /// otherwise, so safe to call every panel tick. Returns true when a refresh happened (the caller
  /// then refills its source list and marks the preview dirty).
  ///
  /// The type belongs in the signature: reloading a different file over the same source/topic
  /// shape can change a field's type while leaving the field COUNT alone, and the type filter
  /// would stay frozen on the previous file's types.
  bool refreshStructureIfChanged(const PJ::sdk::ToolboxHostView& host);

  /// Forces a full re-read regardless of the signature (call before submitting a committed
  /// generator so the input list is current). Returns true if the host was readable.
  bool refresh(const PJ::sdk::ToolboxHostView& host);

  /// Throttled live re-read of ONLY `selected` (cheap for a bounded streaming window), so the
  /// curve + `t0` track the stream. Returns true when new samples were read; an unchanged/paused
  /// source returns false and the preview stays frozen.
  bool refreshSelectedSourceData(const PJ::sdk::ToolboxHostView& host, const std::string& selected);

  [[nodiscard]] const std::vector<std::string>& names() const {
    return series_names_;
  }
  /// True when `name` has cached samples. A name that is listed but absent here means the host
  /// read failed, not that the type was unsupported.
  [[nodiscard]] bool has(const std::string& name) const {
    return series_map_.find(name) != series_map_.end();
  }
  /// Monotonic counter that increments whenever any cached series DATA changes (a full `refresh`
  /// or a live `refreshSelectedSourceData` that read new samples). A consumer caches a derived
  /// view and re-derives only when this value moves. Structure-only no-ops don't bump it.
  [[nodiscard]] std::uint64_t dataRevision() const {
    return data_revision_;
  }
  /// Front timestamp (ns, as double) of `name`, the preview X origin; 0 if absent/empty.
  [[nodiscard]] double t0(const std::string& name) const;
  [[nodiscard]] std::vector<PJ::ChartPoint> points(const std::string& name, std::size_t max_points = 2000) const;

 private:
  struct SeriesData {
    std::vector<double> timestamps;  ///< nanoseconds
    std::vector<double> values;
    /// Everything the live re-read needs to fetch this series again.
    PJ::sdk::FieldHandle handle{};
    PJ::PrimitiveType type{};
  };

  /// Borrowed pointers into a host-owned Arrow series, valid only while the holders that produced
  /// them are alive. Lets a caller inspect sample count and last timestamp WITHOUT paying for the
  /// decode: the streaming path polls far more often than the data moves.
  struct ArrowSeriesRef {
    const std::int64_t* timestamps = nullptr;
    const void* values = nullptr;
    const std::uint8_t* validity = nullptr;
    std::int64_t offset = 0;  ///< of the value column; a BIT offset when the type is kBool
    std::size_t count = 0;
  };

  /// Rejects a shape that is not [int64 timestamp, typed value] and a value column whose Arrow
  /// type contradicts `type` (decoding that would silently misread widths).
  [[nodiscard]] static std::optional<ArrowSeriesRef> openSeries(
      const ArrowSchema* schema, const ArrowArray* array, PJ::PrimitiveType type);

  /// Reads one field and widens it to doubles. Goes through readSeriesArrow rather than
  /// readSeries/valuesAs* because those only cover the ten NUMERIC types (no valuesAsBool(): Arrow
  /// BOOL is a packed bitmap) and expose neither the validity bitmap nor the slice offset.
  /// nullopt when the host read fails, the shape is wrong, or every sample is null.
  [[nodiscard]] static std::optional<SeriesData> materializeSeries(
      const PJ::sdk::ToolboxHostView& host, PJ::sdk::FieldHandle handle, PJ::PrimitiveType type);

  std::unordered_map<std::string, SeriesData> series_map_;
  std::vector<std::string> series_names_;
  std::uint64_t last_catalog_sig_ = UINT64_MAX;  ///< forces the first refresh; guards live ticks
  std::uint64_t data_revision_ = 0;              ///< bumps when cached series DATA changes (see dataRevision)

  // Live-refresh state of the selected source.
  std::string last_source_name_;
  std::size_t last_source_count_ = 0;
  double last_source_last_ts_ = 0.0;
  // Reads every tick while the source MOVES, backing off once it is static, so a huge
  // non-streaming series isn't re-materialized every tick.
  AdaptivePoll source_poll_;
};

}  // namespace toolbox_preview
