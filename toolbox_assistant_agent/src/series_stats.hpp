// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace assistant_agent {

// Summary statistics over one numeric time series. Timestamps are the raw int64
// nanoseconds-since-epoch the datastore stores; durations/rate are derived in
// seconds for a model-friendly report.
struct SeriesStats {
  std::size_t count = 0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  std::int64_t t_start_ns = 0;
  std::int64_t t_end_ns = 0;
  double duration_s = 0.0;
  double rate_hz = 0.0;  // (count - 1) / duration_s, 0 when undefined
};

// One decimated bucket: a time window collapsed to its min/max/mean. `t_rel_s`
// is the bucket's left edge in seconds relative to the first sample, so the
// model sees a compact, human-readable time axis.
struct SeriesBucket {
  double t_rel_s = 0.0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  std::size_t count = 0;
};

// Compute summary stats. `timestamps` and `values` must be equal length and
// timestamps non-decreasing (as the datastore guarantees). Returns a zeroed
// SeriesStats for an empty input.
[[nodiscard]] inline SeriesStats computeStats(
    std::span<const std::int64_t> timestamps, std::span<const double> values) {
  SeriesStats s;
  const std::size_t n = std::min(timestamps.size(), values.size());
  if (n == 0) {
    return s;
  }
  s.count = n;
  s.min = values[0];
  s.max = values[0];
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = values[i];
    s.min = std::min(s.min, v);
    s.max = std::max(s.max, v);
    sum += v;
  }
  s.mean = sum / static_cast<double>(n);
  double sq = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double d = values[i] - s.mean;
    sq += d * d;
  }
  s.stddev = std::sqrt(sq / static_cast<double>(n));
  s.t_start_ns = timestamps[0];
  s.t_end_ns = timestamps[n - 1];
  s.duration_s = static_cast<double>(s.t_end_ns - s.t_start_ns) * 1e-9;
  if (s.duration_s > 0.0 && n > 1) {
    s.rate_hz = static_cast<double>(n - 1) / s.duration_s;
  }
  return s;
}

// Decimate into at most `max_points` equal-width time buckets, preserving the
// per-bucket min and max so spikes survive the downsample (a plain stride would
// alias them away). Buckets with no samples are omitted. When the series
// already has <= max_points samples, each sample becomes its own bucket.
[[nodiscard]] inline std::vector<SeriesBucket> bucketize(
    std::span<const std::int64_t> timestamps, std::span<const double> values, std::size_t max_points) {
  std::vector<SeriesBucket> out;
  const std::size_t n = std::min(timestamps.size(), values.size());
  if (n == 0 || max_points == 0) {
    return out;
  }
  const std::int64_t t0 = timestamps[0];
  const std::int64_t t1 = timestamps[n - 1];
  const std::int64_t span_ns = t1 - t0;

  // Degenerate time span (all samples share a timestamp) or few enough samples:
  // emit one bucket per sample, capped at max_points.
  if (span_ns <= 0 || n <= max_points) {
    const std::size_t limit = std::min(n, max_points);
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
      const double v = values[i];
      out.push_back({static_cast<double>(timestamps[i] - t0) * 1e-9, v, v, v, 1});
    }
    return out;
  }

  out.reserve(max_points);
  const double bucket_ns = static_cast<double>(span_ns) / static_cast<double>(max_points);
  std::size_t idx = 0;
  for (std::size_t b = 0; b < max_points && idx < n; ++b) {
    // Right edge of bucket b (last bucket absorbs the final sample via <= t1).
    const std::int64_t edge =
        (b + 1 == max_points) ? t1 : t0 + static_cast<std::int64_t>(static_cast<double>(b + 1) * bucket_ns);
    SeriesBucket bucket;
    bucket.t_rel_s =
        static_cast<double>(t0 + static_cast<std::int64_t>(static_cast<double>(b) * bucket_ns) - t0) * 1e-9;
    double sum = 0.0;
    bool first = true;
    while (idx < n && (timestamps[idx] <= edge || b + 1 == max_points)) {
      const double v = values[idx];
      if (first) {
        bucket.min = v;
        bucket.max = v;
        first = false;
      } else {
        bucket.min = std::min(bucket.min, v);
        bucket.max = std::max(bucket.max, v);
      }
      sum += v;
      ++bucket.count;
      ++idx;
    }
    if (bucket.count > 0) {
      bucket.mean = sum / static_cast<double>(bucket.count);
      out.push_back(bucket);
    }
  }
  return out;
}

}  // namespace assistant_agent
