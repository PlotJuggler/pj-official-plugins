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

std::optional<SeriesCatalog::ArrowSeriesRef> SeriesCatalog::openSeries(
    const ArrowSchema* schema, const ArrowArray* array, PJ::PrimitiveType type) {
  // Contract: children[0] = int64 timestamps, children[1] = the typed value column.
  if (schema == nullptr || array == nullptr || schema->n_children < 2 || array->n_children < 2) {
    return std::nullopt;
  }
  const ArrowArray* ts_col = array->children[0];
  const ArrowArray* val_col = array->children[1];
  if (ts_col == nullptr || val_col == nullptr || ts_col->n_buffers < 2 || val_col->n_buffers < 2) {
    return std::nullopt;
  }
  // Trust the catalog type only as far as the payload agrees with it: decoding a mismatch
  // reads the buffer at the wrong width, which is silent garbage rather than a failure.
  // This reaches into the SDK's detail:: namespace because the public wrapper that exposes
  // the same mapping (MaterializedSeriesView::type) takes ownership of the holders and
  // hides the raw buffers we still need for validity, bool bits and the slice offset.
  if (PJ::sdk::detail::formatToPrimitiveType(schema->children[1]->format) != type) {
    return std::nullopt;
  }
  const auto* timestamps = static_cast<const std::int64_t*>(ts_col->buffers[1]);
  if (timestamps == nullptr) {
    return std::nullopt;
  }
  ArrowSeriesRef ref;
  ref.timestamps = timestamps + ts_col->offset;  // the timestamp column has its own offset
  ref.values = val_col->buffers[1];
  ref.validity = static_cast<const std::uint8_t*>(val_col->buffers[0]);
  ref.offset = val_col->offset;
  ref.count = static_cast<std::size_t>(val_col->length);
  return ref;
}

std::optional<SeriesCatalog::SeriesData> SeriesCatalog::materializeSeries(
    const PJ::sdk::ToolboxHostView& host, PJ::sdk::FieldHandle handle, PJ::PrimitiveType type) {
  PJ::sdk::ArrowSchemaHolder schema;
  PJ::sdk::ArrowArrayHolder array;
  if (!host.readSeriesArrow(handle, schema.out(), array.out())) {
    return std::nullopt;
  }
  const std::optional<ArrowSeriesRef> ref = openSeries(schema.get(), array.get(), type);
  if (!ref) {
    return std::nullopt;
  }
  SeriesData data;
  data.handle = handle;
  data.type = type;
  if (!decodeSeriesAsDouble(
          type, ref->values, ref->validity, ref->offset, ref->timestamps, ref->count, data.timestamps, data.values)) {
    return std::nullopt;
  }
  if (data.timestamps.empty()) {
    return std::nullopt;  // no samples, or every one of them was null
  }
  return data;
}

bool SeriesCatalog::refresh(const PJ::sdk::ToolboxHostView& host) {
  auto catalog = host.catalogSnapshot();
  if (!catalog) {
    return false;
  }
  series_map_.clear();
  series_names_.clear();
  const auto all_fields = catalog->fields();
  for (const auto& topic : catalog->topics()) {
    const std::string topic_name(PJ::sdk::toStringView(topic.name));
    for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
      const auto& f = all_fields[fi];
      // Key with markerSeriesKey so it matches the plot overlay's per-series key exactly
      // (so per-series markers land on the right curve). It is the canonical "topic/field"
      // series key, not marker-specific data.
      std::string name = PJ::sdk::markerSeriesKey(topic_name, PJ::sdk::toStringView(f.name));
      const PJ::PrimitiveType type = PJ::sdk::fromAbiType(f.type);
      // Filter BEFORE the read, not after: a text topic would otherwise be materialised in
      // full only to be thrown away.
      if (!isPlottableType(type)) {
        continue;
      }
      series_names_.push_back(name);
      if (std::optional<SeriesData> data = materializeSeries(host, f.handle, type)) {
        series_map_[name] = std::move(*data);
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
  // Field handle + TYPE, not just the per-topic count. Reloading a different file over the
  // same source/topic shape can retype a field without moving any count, and the source
  // list is now type-filtered — so a signature blind to types would leave the filter
  // frozen on the previous file's types.
  for (const auto& f : catalog->fields()) {
    sig = (sig ^ ((static_cast<std::uint64_t>(f.handle.id) << 8) | static_cast<std::uint64_t>(f.type))) *
          1099511628211ull;
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
  const auto cached_it = series_map_.find(selected);
  if (cached_it == series_map_.end()) {
    return false;  // never materialised (unreadable, or not snapshotted yet)
  }
  if (selected != last_source_name_) {
    last_source_name_ = selected;  // new selection -> force a fresh read below
    last_source_count_ = 0;
    last_source_last_ts_ = 0.0;
    source_poll_.reset();
  }
  // Open the Arrow payload but DON'T decode yet: this runs at the panel tick rate while
  // most polls find nothing new, and decoding first would allocate and fill two vectors
  // the size of the whole series only to throw them away.
  PJ::sdk::ArrowSchemaHolder schema;
  PJ::sdk::ArrowArrayHolder array;
  if (!host.readSeriesArrow(cached_it->second.handle, schema.out(), array.out())) {
    return false;
  }
  const std::optional<ArrowSeriesRef> ref = openSeries(schema.get(), array.get(), cached_it->second.type);
  if (!ref || ref->count == 0) {
    return false;
  }
  const double last_ts = static_cast<double>(ref->timestamps[ref->count - 1]);
  if (ref->count == last_source_count_ && last_ts == last_source_last_ts_) {
    source_poll_.observe(false);  // unchanged -> let the cadence back off
    return false;                 // no new samples -> leave the (frozen) preview as is
  }

  SeriesData data;
  data.handle = cached_it->second.handle;
  data.type = cached_it->second.type;
  if (!decodeSeriesAsDouble(
          data.type, ref->values, ref->validity, ref->offset, ref->timestamps, ref->count, data.timestamps,
          data.values)) {
    return false;
  }
  source_poll_.observe(true);  // moving -> keep reading every tick for a smooth follow
  last_source_count_ = ref->count;
  last_source_last_ts_ = last_ts;
  cached_it->second = std::move(data);
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
