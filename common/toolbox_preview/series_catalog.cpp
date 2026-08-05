// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "toolbox_preview/series_catalog.hpp"

#include <pj_base/builtin/plot_markers.hpp>  // sdk::markerSeriesKey
#include <utility>

namespace toolbox_preview {

std::vector<PJ::ChartPoint> downsampleToChart(
    const std::vector<double>& timestamps_ns, const std::vector<double>& values, std::size_t max_points) {
  std::vector<PJ::ChartPoint> pts;
  if (timestamps_ns.empty() || timestamps_ns.size() != values.size()) {
    return pts;
  }
  const double t0 = timestamps_ns.front();
  const std::size_t n = timestamps_ns.size();
  const std::size_t stride = (max_points > 0 && n > max_points) ? (n / max_points) : 1;
  pts.reserve(n / stride + 1);
  for (std::size_t i = 0; i < n; i += stride) {
    pts.push_back(PJ::ChartPoint{(timestamps_ns[i] - t0) / 1e9, values[i]});
  }
  return pts;
}

bool SeriesCatalog::refresh(const PJ::sdk::ToolboxHostView& host) {
  auto catalog = host.catalogSnapshot();
  if (!catalog) {
    return false;
  }
  series_map_.clear();
  series_names_.clear();
  field_handles_.clear();
  const auto all_fields = catalog->fields();
  for (const auto& topic : catalog->topics()) {
    const std::string topic_name(PJ::sdk::toStringView(topic.name));
    for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
      const auto& f = all_fields[fi];
      // Key with markerSeriesKey so it matches the plot overlay's per-series key exactly
      // (so per-series markers land on the right curve). It is the canonical "topic/field"
      // series key, not marker-specific data.
      std::string name = PJ::sdk::markerSeriesKey(topic_name, PJ::sdk::toStringView(f.name));
      series_names_.push_back(name);
      auto series = host.readSeries(f.handle);
      if (series && series->type() == PJ::PrimitiveType::kFloat64) {
        const auto ts = series->timestamps();
        const double* values = series->valuesAsFloat64();
        const size_t count = ts.size();
        if (values != nullptr) {
          series_map_[name] = buildSeriesData(ts, values, count);
          field_handles_[name] = f.handle;  // cached so the tick path re-reads ONE series
        }
      }
    }
  }
  ++data_revision_;  // every series was re-read → invalidate any cached derived view
  return true;
}

bool SeriesCatalog::refreshStructureIfChanged(const PJ::sdk::ToolboxHostView& host) {
  auto catalog = host.catalogSnapshot();
  if (!catalog) {
    return false;
  }
  std::uint64_t sig = 1469598103934665603ull;  // FNV offset basis as a seed
  for (const auto& ds : catalog->dataSources()) {
    sig = (sig ^ ds.handle.id) * 1099511628211ull;
  }
  for (const auto& topic : catalog->topics()) {
    sig = (sig ^ (static_cast<std::uint64_t>(topic.source.id) << 32 | topic.field_count)) * 1099511628211ull;
  }
  if (sig == last_catalog_sig_) {
    return false;
  }
  last_catalog_sig_ = sig;
  return refresh(host);
}

bool SeriesCatalog::refreshSelectedSourceData(const PJ::sdk::ToolboxHostView& host, const std::string& selected) {
  // The cadence policy lives in source_poll_ (read every tick while the source moves, back
  // off when it is static); this method is just the read mechanism.
  if (!source_poll_.shouldRead()) {
    return false;
  }
  if (selected.empty()) {
    return false;
  }
  const auto handle_it = field_handles_.find(selected);
  if (handle_it == field_handles_.end()) {
    return false;  // not a readable float64 series (or not snapshotted yet)
  }
  if (selected != last_source_name_) {
    last_source_name_ = selected;  // new selection -> force a fresh read below
    last_source_count_ = 0;
    last_source_last_ts_ = 0.0;
    source_poll_.reset();
  }
  auto series = host.readSeries(handle_it->second);
  if (!series || series->type() != PJ::PrimitiveType::kFloat64) {
    return false;
  }
  const auto ts = series->timestamps();
  const double* values = series->valuesAsFloat64();
  const size_t count = ts.size();
  if (values == nullptr || count == 0) {
    return false;
  }
  const double last_ts = static_cast<double>(ts[count - 1]);
  if (count == last_source_count_ && last_ts == last_source_last_ts_) {
    source_poll_.observe(false);  // unchanged -> let the cadence back off
    return false;                 // no new samples -> leave the (frozen) preview as is
  }
  source_poll_.observe(true);  // moving -> keep reading every tick for a smooth follow
  last_source_count_ = count;
  last_source_last_ts_ = last_ts;
  series_map_[selected] = buildSeriesData(ts, values, count);
  ++data_revision_;  // new samples for the selected source → invalidate the cached curve/overlay
  return true;
}

double SeriesCatalog::t0(const std::string& name) const {
  const auto it = series_map_.find(name);
  if (it == series_map_.end() || it->second.timestamps.empty()) {
    return 0.0;
  }
  return it->second.timestamps.front();
}

std::vector<PJ::ChartPoint> SeriesCatalog::points(const std::string& name, std::size_t max_points) const {
  const auto it = series_map_.find(name);
  if (it == series_map_.end()) {
    return {};
  }
  // Stride-decimation is O(max_points), not O(N) — cheap enough to run on every panel tick.
  return downsampleToChart(it->second.timestamps, it->second.values, max_points);
}

}  // namespace toolbox_preview
