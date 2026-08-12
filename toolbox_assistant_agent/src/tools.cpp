// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The assistant's tool executors. Each is a pure function of (args, host views)
// and returns a ToolResult whose `content` the model reads. Every failure is
// returned as data (ok=false) — exceptions must never cross the plugin ABI, so
// nothing here throws out.

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "luau_transform.hpp"
#include "series_stats.hpp"
#include "tool_registry.hpp"

namespace assistant_agent {
namespace {

using nlohmann::json;

const char* markerKindName(PJ::sdk::MarkerKind k) {
  switch (k) {
    case PJ::sdk::MarkerKind::kRegion:
      return "regions";
    case PJ::sdk::MarkerKind::kEvent:
      return "events";
    case PJ::sdk::MarkerKind::kValueBand:
      return "value_bands";
    case PJ::sdk::MarkerKind::kLabel:
      return "labels";
  }
  return "other";
}

// What the host ACTUALLY published for a marker set, read back out of the
// object store.
//
// This exists because the model cannot see the plot. Told only "created", it
// has no way to distinguish twelve shaded regions from four thousand vertical
// lines that merge into a wall — and it will report both as a success, because
// from where it sits they are identical. The breakdown by kind is the part that
// carries the meaning: "4182 events" is a wall, "12 regions" is an annotation.
//
// A marker topic holds ONE serialized PlotMarkers blob (MarkerService pushes the
// whole set at Timestamp{0} and republishes it on every change), so entryCount()
// would report 1 no matter how many markers there are — the payload has to be
// decoded. Same read path the anomaly detector uses for its live preview.
//
// Returns a null json when the optional read service is absent: the answer then
// carries no count, which is strictly better than the tool failing.
json publishedMarkerSummary(const ToolContext& ctx, const std::vector<std::string>& object_topics) {
  if (!ctx.objects.valid()) {
    return nullptr;
  }
  std::size_t total = 0;
  std::map<std::string, std::size_t> by_kind;
  bool any_span = false;
  PJ::Timestamp t_min = 0;
  PJ::Timestamp t_max = 0;
  for (const auto& name : object_topics) {
    const std::optional<PJ::sdk::ObjectTopicHandle> handle = ctx.objects.lookupTopic(name);
    if (!handle) {
      continue;
    }
    const PJ::Expected<PJ::sdk::ObjectBytes> bytes = ctx.objects.readLatestAt(*handle, PJ::Timestamp{0});
    if (!bytes || bytes->empty()) {
      continue;
    }
    const PJ::Span<const uint8_t> view = bytes->view();
    const PJ::Expected<PJ::sdk::PlotMarkers> decoded = PJ::deserializePlotMarkers(view.data(), view.size());
    if (!decoded) {
      continue;
    }
    for (const auto& m : decoded->markers) {
      ++total;
      ++by_kind[markerKindName(m.kind)];
      const PJ::Timestamp lo = m.t_start;
      const PJ::Timestamp hi = m.kind == PJ::sdk::MarkerKind::kRegion ? m.t_end : m.t_start;
      if (!any_span) {
        t_min = lo;
        t_max = hi;
        any_span = true;
      } else {
        t_min = std::min(t_min, lo);
        t_max = std::max(t_max, hi);
      }
    }
  }
  json out = {{"markers_created", total}};
  if (!by_kind.empty()) {
    out["by_kind"] = by_kind;
  }
  if (any_span) {
    out["span_s"] = static_cast<double>(t_max - t_min) * 1e-9;
  }
  return out;
}

// Cap on a single tool response handed back to the model, so a wide catalog or
// a long series can't blow the context window. read_series coarsens to fit.
constexpr std::size_t kMaxResponseBytes = 16 * 1024;

const char* primitiveTypeName(PJ::PrimitiveType t) {
  switch (t) {
    case PJ::PrimitiveType::kFloat32:
      return "float32";
    case PJ::PrimitiveType::kFloat64:
      return "float64";
    case PJ::PrimitiveType::kInt8:
      return "int8";
    case PJ::PrimitiveType::kInt16:
      return "int16";
    case PJ::PrimitiveType::kInt32:
      return "int32";
    case PJ::PrimitiveType::kInt64:
      return "int64";
    case PJ::PrimitiveType::kUint8:
      return "uint8";
    case PJ::PrimitiveType::kUint16:
      return "uint16";
    case PJ::PrimitiveType::kUint32:
      return "uint32";
    case PJ::PrimitiveType::kUint64:
      return "uint64";
    case PJ::PrimitiveType::kBool:
      return "bool";
    case PJ::PrimitiveType::kString:
      return "string";
    default:
      return "unspecified";
  }
}

// Dataset name for each topic INDEX, or empty when the host reports no data
// sources (the SDK's test store is one such host, so every unit test exercises
// the degraded path). Topics are laid out contiguously per source, so this is a
// table build rather than a search.
//
// It matters because PJ4 can hold several datasets at once — two runs of the
// same robot is the ordinary case — and a flat topic list makes them
// indistinguishable. Without it the model can neither offer to compare two runs
// nor avoid mixing them, for the same reason: it does not know there are two.
std::map<std::uint32_t, std::string> datasetByTopicIndex(const PJ::sdk::CatalogSnapshot& catalog) {
  std::map<std::uint32_t, std::string> out;
  const auto sources = catalog.dataSources();
  if (sources.size() < 2) {
    return out;  // one source (or none) adds no information worth the tokens
  }
  for (const auto& src : sources) {
    const std::string name(PJ::sdk::toStringView(src.name));
    for (std::uint32_t i = 0; i < src.topic_count; ++i) {
      out[src.first_topic + i] = name;
    }
  }
  return out;
}

// Join a topic name and a field path into the canonical curve path. Hosts
// differ on whether leaf field names carry a leading '/' (the plot-markers
// host does, main does not), so tolerate both — a naive '+ "/" +' join emits
// "topic//field", which the marker engine's series() lookup rejects.
std::string joinSeriesPath(std::string_view topic, std::string_view field) {
  std::string path(topic);
  if (field.empty()) {
    return path;
  }
  if (field.front() != '/') {
    path.push_back('/');
  }
  path.append(field);
  return path;
}

// Collapse '/' runs in a model-supplied series path. The model echoes paths
// verbatim from earlier tool output (possibly from an older, doubling build),
// so accept "topic//field" as "topic/field" everywhere a path comes in.
std::string canonicalSeriesPath(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    if (ch == '/' && !out.empty() && out.back() == '/') {
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

// A resolved "topic/field" curve path: the field handle plus the owning topic
// name.
struct ResolvedSeries {
  PJ::sdk::FieldHandle handle;
  std::string topic;
  std::string path;  // the full canonical path, which may differ from what was asked for
};

// Split a curve path into its '/'-separated segments, ignoring empty ones so
// leading or doubled slashes don't produce phantom segments.
std::vector<std::string_view> pathSegments(std::string_view path) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') {
      ++i;
    }
    const std::size_t start = i;
    while (i < path.size() && path[i] != '/') {
      ++i;
    }
    if (i > start) {
      out.push_back(path.substr(start, i - start));
    }
  }
  return out;
}

// True when `want`'s segments appear as a contiguous run inside `have`'s. This
// is what makes an abbreviated path resolvable: "test/sin" and "sin/value" and
// bare "sin" all match "test/sin/value". Matching whole segments (rather than
// substrings) is deliberate — "test/si" must not resolve to "test/sin/value".
bool segmentsContain(const std::vector<std::string_view>& have, const std::vector<std::string_view>& want) {
  if (want.empty() || want.size() > have.size()) {
    return false;
  }
  for (std::size_t off = 0; off + want.size() <= have.size(); ++off) {
    bool all = true;
    for (std::size_t k = 0; k < want.size(); ++k) {
      if (have[off + k] != want[k]) {
        all = false;
        break;
      }
    }
    if (all) {
      return true;
    }
  }
  return false;
}

// Outcome of a path lookup. When nothing resolves, `candidates` carries the
// near misses so the caller can put them in the error — a model that gets told
// what the real paths are corrects on the spot, instead of spending a whole
// extra round-trip asking the catalog.
struct SeriesLookup {
  std::optional<ResolvedSeries> resolved;
  std::vector<std::string> candidates;
  bool ambiguous = false;  // several paths matched; refusing to guess between them
};

// Cap on how many near misses we name. The error text is fed back to the model
// and then re-sent on every later round-trip of the turn, so an unbounded list
// would be paid for repeatedly.
constexpr std::size_t kMaxCandidates = 10;

// Resolve one "topic/field" curve path (joinSeriesPath convention, the same the
// rest of PJ4 uses) by scanning the catalog — models call read_series
// repeatedly per turn, so avoid materializing a full path index.
//
// An exact match always wins. Failing that, an abbreviated path resolves only
// when exactly one series matches: with several the answer is the candidate
// list, never a guess, because silently picking one would attach a transform to
// the wrong signal and look like it worked.
SeriesLookup resolveSeriesPath(const PJ::sdk::CatalogSnapshot& catalog, const std::string& series) {
  SeriesLookup out;
  const auto want = pathSegments(series);
  auto topics = catalog.topics();
  auto fields = catalog.fields();

  std::optional<ResolvedSeries> fuzzy;
  for (const auto& topic : topics) {
    const auto topic_name = PJ::sdk::toStringView(topic.name);
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      const std::string full = joinSeriesPath(topic_name, PJ::sdk::toStringView(fields[idx].name));
      if (full == series) {
        out.resolved = ResolvedSeries{fields[idx].handle, std::string(topic_name), full};
        return out;  // exact beats everything; stop looking
      }
      if (segmentsContain(pathSegments(full), want)) {
        if (out.candidates.size() < kMaxCandidates) {
          out.candidates.push_back(full);
        }
        if (!fuzzy) {
          fuzzy = ResolvedSeries{fields[idx].handle, std::string(topic_name), full};
        } else {
          out.ambiguous = true;
        }
      }
    }
  }
  if (!out.ambiguous && fuzzy) {
    out.resolved = std::move(fuzzy);
    out.candidates.clear();
  }
  return out;
}

// The error a failed lookup should produce: self-contained, so the model can
// fix the path from the message alone.
std::string seriesLookupError(const std::string& series, const SeriesLookup& lookup) {
  if (lookup.ambiguous) {
    std::string msg = "'" + series + "' is ambiguous; it matches ";
    for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
      msg += (i != 0 ? ", " : "");
      msg += "'" + lookup.candidates[i] + "'";
    }
    msg += ". Use the full path.";
    return msg;
  }
  return "unknown series '" + series +
         "'. If you expected it to exist, call list_topics (with a filter) or describe_topic once to check before "
         "concluding it is missing.";
}

// Materialize a numeric field into parallel timestamp + double columns,
// coercing any numeric column type to double. Returns false for a non-numeric
// column (string/bool) or an empty/mismatched read.
bool readSeriesDoubles(
    const PJ::sdk::MaterializedSeriesView& view, std::vector<std::int64_t>& out_ts, std::vector<double>& out_vals) {
  const std::size_t n = view.rowCount();
  auto ts = view.timestamps();
  if (n == 0 || ts.size() != n) {
    return false;
  }
  out_ts.assign(ts.begin(), ts.end());
  out_vals.resize(n);

  auto copy_from = [&](const auto* p) -> bool {
    if (p == nullptr) {
      return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
      out_vals[i] = static_cast<double>(p[i]);
    }
    return true;
  };

  switch (view.type()) {
    case PJ::PrimitiveType::kFloat64:
      return copy_from(view.valuesAsFloat64());
    case PJ::PrimitiveType::kFloat32:
      return copy_from(view.valuesAsFloat32());
    case PJ::PrimitiveType::kInt8:
      return copy_from(view.valuesAsInt8());
    case PJ::PrimitiveType::kInt16:
      return copy_from(view.valuesAsInt16());
    case PJ::PrimitiveType::kInt32:
      return copy_from(view.valuesAsInt32());
    case PJ::PrimitiveType::kInt64:
      return copy_from(view.valuesAsInt64());
    case PJ::PrimitiveType::kUint8:
      return copy_from(view.valuesAsUint8());
    case PJ::PrimitiveType::kUint16:
      return copy_from(view.valuesAsUint16());
    case PJ::PrimitiveType::kUint32:
      return copy_from(view.valuesAsUint32());
    case PJ::PrimitiveType::kUint64:
      return copy_from(view.valuesAsUint64());
    default:
      return false;  // bool/string aren't plottable numeric series
  }
}

// --- executors -------------------------------------------------------------

ToolResult listTopics(const json& args, ToolContext& ctx) {
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  const std::string filter = args.value("filter", std::string{});
  // Clamped, not just defaulted: this response is re-sent on every remaining
  // round-trip of the turn, so an unbounded list would be paid for repeatedly.
  // read_series bounds its payload the same way. The truncation note below
  // still tells the model how to narrow the search.
  const int limit = std::clamp(args.value("limit", 100), 1, 500);

  // Scoping to one dataset is what makes this usable with several loaded: the
  // same topic names repeat across runs, so a name filter alone returns both.
  const std::string dataset_filter = args.value("dataset", std::string{});

  json topics = json::array();
  int matched = 0;
  int shown = 0;
  auto all = catalog->topics();
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(*catalog);
  for (std::uint32_t ti = 0; ti < all.size(); ++ti) {
    const auto& topic = all[ti];
    const std::string name(PJ::sdk::toStringView(topic.name));
    if (!filter.empty() && name.find(filter) == std::string::npos) {
      continue;
    }
    const auto ds = topic_dataset.find(ti);
    const std::string dataset = ds != topic_dataset.end() ? ds->second : std::string{};
    if (!dataset_filter.empty() && dataset.find(dataset_filter) == std::string::npos) {
      continue;
    }
    ++matched;
    if (shown < limit) {
      json entry = {{"topic", name}, {"fields", topic.field_count}};
      if (!dataset.empty()) {
        entry["dataset"] = dataset;
      }
      topics.push_back(entry);
      ++shown;
    }
  }
  json out = {{"count", matched}, {"shown", shown}, {"topics", topics}};
  if (matched > shown) {
    out["note"] = "truncated to " + std::to_string(shown) + " of " + std::to_string(matched) +
                  "; refine with a filter or raise limit";
  }
  return ToolResult::success(out.dump());
}

ToolResult describeTopic(const json& args, ToolContext& ctx) {
  if (!args.contains("topic") || !args["topic"].is_string()) {
    return ToolResult::failure("describe_topic requires a string 'topic'");
  }
  const std::string want = args["topic"].get<std::string>();
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  auto topics = catalog->topics();
  auto fields = catalog->fields();
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(*catalog);
  for (std::uint32_t ti = 0; ti < topics.size(); ++ti) {
    const auto& topic = topics[ti];
    if (std::string(PJ::sdk::toStringView(topic.name)) != want) {
      continue;
    }
    json field_arr = json::array();
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      const std::string leaf(PJ::sdk::toStringView(fields[idx].name));
      json entry = {
          {"field", leaf},
          {"path", joinSeriesPath(want, leaf)},
          {"type", primitiveTypeName(PJ::sdk::fromAbiType(fields[idx].type))}};
      field_arr.push_back(entry);
    }
    json out = {{"topic", want}, {"fields", field_arr}};
    if (auto it = topic_dataset.find(ti); it != topic_dataset.end()) {
      out["dataset"] = it->second;
    }
    return ToolResult::success(out.dump());
  }
  return ToolResult::failure("no topic named '" + want + "' (use list_topics)");
}

json statsToJson(const SeriesStats& s) {
  return {{"count", s.count},           {"min", s.min},        {"max", s.max}, {"mean", s.mean}, {"stddev", s.stddev},
          {"duration_s", s.duration_s}, {"rate_hz", s.rate_hz}};
}

// How a multi-input transform would fare BEFORE anything is installed.
//
// PJ4 joins the inputs of a multi-input transform on EXACT timestamp equality
// (pj_datastore run_mimo_incremental). Inputs that share no timestamps produce a
// series with zero points — created "successfully", drawn as nothing, with no
// error anywhere. That is not a rare corner: two recordings of the same robot
// are exactly that, and comparing two runs is a perfectly reasonable thing to
// ask for.
//
// So this measures the real intersection rather than guessing from sample rates
// (two 100 Hz series can still share nothing if one is offset by half a sample —
// a rate comparison would call that compatible and be wrong).
struct JoinForecast {
  bool checked = false;            // false when an input could not be read; draw no conclusion
  std::size_t shared = 0;          // timestamps common to every input
  std::size_t smallest = 0;        // rows in the shortest input, the ceiling for `shared`
  std::vector<std::string> rates;  // "path (100.0 Hz, 200 samples)" per input, for the message
};

JoinForecast forecastJoin(
    const ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const std::vector<std::string>& inputs) {
  JoinForecast out;
  std::vector<std::int64_t> common;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    auto lookup = resolveSeriesPath(catalog, inputs[i]);
    if (!lookup.resolved) {
      return out;
    }
    auto view = ctx.host.readSeries(lookup.resolved->handle);
    if (!view) {
      return out;
    }
    std::vector<std::int64_t> ts;
    std::vector<double> vals;
    if (!readSeriesDoubles(*view, ts, vals)) {
      return out;  // a non-numeric input fails later, on its own terms
    }
    const SeriesStats stats = computeStats(ts, vals);
    std::ostringstream rate;
    rate << inputs[i] << " (" << std::fixed << std::setprecision(1) << stats.rate_hz << " Hz, " << ts.size()
         << " samples)";
    out.rates.push_back(rate.str());
    out.smallest = (i == 0) ? ts.size() : std::min(out.smallest, ts.size());

    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    if (i == 0) {
      common = std::move(ts);
    } else {
      std::vector<std::int64_t> next;
      std::set_intersection(common.begin(), common.end(), ts.begin(), ts.end(), std::back_inserter(next));
      common = std::move(next);
    }
  }
  out.checked = true;
  out.shared = common.size();
  return out;
}

// Cap on how many series one call may read. Stats are small and fixed-size per
// series, so this is not about the response cap — it is about not turning a
// single tool call into an unbounded scan of the whole dataset.
constexpr std::size_t kMaxBatchPaths = 32;

// Collect the requested paths from either a bare string or an array.
//
// The parameter is named `paths` rather than `series` on purpose. The object
// store holds point clouds, images and occupancy grids behind the same read
// view, and when this call learns to summarize those too, "series" would be the
// wrong word for what it takes. The model learns the name from the description,
// so renaming it later is more expensive than choosing it now. `series` still
// works, undocumented, so a conversation already in flight does not break.
std::vector<std::string> requestedPaths(const json& args) {
  std::vector<std::string> out;
  for (const char* key : {"paths", "series"}) {
    if (!args.contains(key)) {
      continue;
    }
    const json& v = args[key];
    if (v.is_string()) {
      out.push_back(canonicalSeriesPath(v.get<std::string>()));
    } else if (v.is_array()) {
      for (const auto& e : v) {
        if (e.is_string()) {
          out.push_back(canonicalSeriesPath(e.get<std::string>()));
        }
      }
    }
    if (!out.empty()) {
      break;
    }
  }
  return out;
}

// Read one series into timestamps + doubles, or return why not. Shared by both
// modes so a batch entry and a single read fail with the same wording.
struct SeriesRead {
  bool ok = false;
  std::string error;
  std::string path;  // the resolved path, which may differ from what was asked
  std::vector<std::int64_t> ts;
  std::vector<double> vals;
};

SeriesRead readOne(const PJ::sdk::CatalogSnapshot& catalog, ToolContext& ctx, const std::string& want) {
  SeriesRead r;
  r.path = want;
  auto lookup = resolveSeriesPath(catalog, want);
  if (!lookup.resolved) {
    r.error = seriesLookupError(want, lookup);
    return r;
  }
  r.path = lookup.resolved->path;
  auto view = ctx.host.readSeries(lookup.resolved->handle);
  if (!view) {
    r.error = "read failed for '" + want + "': " + view.error();
    return r;
  }
  if (!readSeriesDoubles(*view, r.ts, r.vals)) {
    r.error = "series '" + want + "' is not a numeric time series";
    return r;
  }
  r.ok = true;
  return r;
}

ToolResult readSeriesTool(const json& args, ToolContext& ctx) {
  const std::vector<std::string> paths = requestedPaths(args);
  if (paths.empty()) {
    return ToolResult::failure(
        "read_series requires 'paths': one topic/field path, or an array of them to read several in "
        "a single call");
  }
  if (paths.size() > kMaxBatchPaths) {
    return ToolResult::failure(
        "read_series takes at most " + std::to_string(kMaxBatchPaths) + " paths per call, got " +
        std::to_string(paths.size()));
  }
  const std::string mode = args.value("mode", std::string("stats"));

  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }

  if (mode == "stats") {
    // One entry per requested path, each carrying its own error. A single typo
    // must not cost the whole round trip — which is the entire point of asking
    // for several at once.
    json arr = json::array();
    std::size_t failed = 0;
    for (const auto& want : paths) {
      SeriesRead r = readOne(*catalog, ctx, want);
      if (!r.ok) {
        ++failed;
        arr.push_back({{"series", want}, {"error", r.error}});
        continue;
      }
      arr.push_back({{"series", r.path}, {"stats", statsToJson(computeStats(r.ts, r.vals))}});
    }
    // A lone path keeps the shape it has always had, so nothing that worked
    // before starts reading differently.
    if (paths.size() == 1) {
      const json& only = arr.front();
      return only.contains("error") ? ToolResult::failure(only["error"].get<std::string>())
                                    : ToolResult::success(only.dump());
    }
    json out = {{"count", arr.size()}, {"read", arr}};
    if (failed != 0) {
      out["failed"] = failed;
    }
    return ToolResult::success(out.dump());
  }

  if (mode == "buckets") {
    // Deliberately single-series. Buckets are the expensive, variable payload,
    // and fitting several into one response means coarsening each until the set
    // fits — degrading exactly the thing buckets exist to show. Asking for them
    // one at a time keeps each one at full usable resolution.
    if (paths.size() != 1) {
      return ToolResult::failure(
          "mode 'buckets' reads one series at a time: fitting several shapes in one response would "
          "coarsen each of them past usefulness. Batch 'stats' instead, then request buckets for the "
          "series worth looking at.");
    }
    SeriesRead r = readOne(*catalog, ctx, paths.front());
    if (!r.ok) {
      return ToolResult::failure(r.error);
    }
    const json stats_json = statsToJson(computeStats(r.ts, r.vals));
    // Coarsen until the serialized payload fits the response cap so spiky data
    // stays representable without overrunning the model's context.
    std::size_t max_points = static_cast<std::size_t>(std::clamp(args.value("max_points", 200), 1, 500));
    for (;;) {
      auto buckets = bucketize(r.ts, r.vals, max_points);
      json bucket_arr = json::array();
      for (const auto& b : buckets) {
        bucket_arr.push_back({{"t", b.t_rel_s}, {"min", b.min}, {"max", b.max}, {"mean", b.mean}, {"n", b.count}});
      }
      json out = {{"series", r.path}, {"stats", stats_json}, {"buckets", bucket_arr}};
      std::string dumped = out.dump();
      if (dumped.size() <= kMaxResponseBytes || max_points <= 16) {
        if (dumped.size() > kMaxResponseBytes) {
          out["note"] = "coarsened to fit the response cap";
          dumped = out.dump();
        }
        return ToolResult::success(dumped);
      }
      max_points /= 2;
    }
  }
  return ToolResult::failure("unknown mode '" + mode + "' (use 'stats' or 'buckets')");
}

ToolResult createDerivedSeries(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot create series)");
  }
  if (!args.contains("name") || !args["name"].is_string() || args["name"].get<std::string>().empty()) {
    return ToolResult::failure("create_derived_series requires a non-empty string 'name'");
  }
  if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty()) {
    return ToolResult::failure("create_derived_series requires a non-empty 'inputs' array of topic/field paths");
  }
  const bool has_expr = args.contains("expression") && args["expression"].is_string();
  const bool has_body = args.contains("body") && args["body"].is_string();
  if (!has_expr && !has_body) {
    return ToolResult::failure(
        "create_derived_series requires 'expression' (a stateless Luau expression over value/v1../time, "
        "e.g. 'value * 2') OR 'body' (full Luau statements ending in return, with optional 'global' state)");
  }
  const std::string name = args["name"].get<std::string>();
  // One node can produce SEVERAL series. The host has always accepted a span of
  // outputs and our wrapper already forwards every value the body returns
  // (MULTRET), so the single-output limit was ours alone — it forced three
  // separate nodes, each recomputing the same intermediate, for something like
  // roll/pitch/yaw out of one quaternion. Defaults to [name], so nothing that
  // worked before changes.
  std::vector<std::string> outputs;
  if (args.contains("outputs") && args["outputs"].is_array()) {
    for (const auto& o : args["outputs"]) {
      if (o.is_string() && !o.get<std::string>().empty()) {
        outputs.push_back(o.get<std::string>());
      }
    }
  }
  if (outputs.empty()) {
    outputs.push_back(name);
  }
  std::vector<std::string> inputs;
  for (const auto& in : args["inputs"]) {
    if (in.is_string()) {
      inputs.push_back(canonicalSeriesPath(in.get<std::string>()));
    }
  }
  if (inputs.empty()) {
    return ToolResult::failure("'inputs' contained no string paths");
  }
  // Resolve every input against the catalog before handing it to the host.
  // Two reasons: an abbreviated path becomes the real one here (no correction
  // round-trip), and a path that names nothing fails loudly instead of
  // installing a transform whose input never matches — which produces an empty
  // curve and looks like it worked.
  JoinForecast forecast;
  if (auto catalog = ctx.host.catalogSnapshot()) {
    for (auto& in : inputs) {
      auto lookup = resolveSeriesPath(*catalog, in);
      if (!lookup.resolved) {
        return ToolResult::failure(seriesLookupError(in, lookup));
      }
      in = lookup.resolved->path;
    }
    // The same failure the resolution above guards against — an empty curve that
    // looks like it worked — reached the other way: inputs that all exist but
    // share no timestamps. Refuse to build it, and point at what does work,
    // because wanting to relate two recordings is legitimate even when a joined
    // series cannot express it.
    if (inputs.size() > 1) {
      forecast = forecastJoin(ctx, *catalog, inputs);
      if (forecast.checked && forecast.shared == 0) {
        std::string why = "these inputs share no timestamps, so the joined series would have 0 points: ";
        for (std::size_t i = 0; i < forecast.rates.size(); ++i) {
          why += (i == 0 ? "" : ", ") + forecast.rates[i];
        }
        why +=
            ". Multi-input transforms join on exact timestamp equality, so they only work on series recorded on the "
            "same clock. To relate series that are not (two runs, two devices), read_series each one and compare the "
            "statistics, or tell the user to plot them together.";
        return ToolResult::failure(why);
      }
    }
  }
  const std::size_t num_extra = inputs.size() - 1;
  // `body` (full statements, for stateful transforms like a derivative) wins;
  // otherwise wrap the stateless `expression` in a return. `global` runs once
  // per instance and its locals persist across samples (PJ3 global semantics).
  const std::string global = args.value("global", std::string{});
  const std::string body =
      has_body ? args["body"].get<std::string>() : "    return (" + args["expression"].get<std::string>() + ")";
  // Compile-check before installing, so a bad expression is a clean tool error.
  // The host's validator instantiates the class it expects to be named
  // "__validate__" (DataProcessorService::validateScript), so the validation
  // script MUST use that id — while the install script keeps the real name.
  const std::string validate_script = buildLuauTransform("__validate__", "__validate__", global, body, num_extra);
  if (auto v = ctx.dp.validateScript("transform", ctx.language, validate_script); !v) {
    return ToolResult::failure("invalid expression: " + v.error());
  }
  const std::string script = buildLuauTransform(name, name, global, body, num_extra);

  std::vector<std::string_view> in_views(inputs.begin(), inputs.end());
  std::vector<std::string_view> out_views(outputs.begin(), outputs.end());
  auto status = ctx.dp.createTransform(
      name, PJ::Span<const std::string_view>(in_views.data(), in_views.size()),
      PJ::Span<const std::string_view>(out_views.data(), out_views.size()), script, "{}");
  if (!status) {
    return ToolResult::failure("create failed: " + status.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  // Report the readable series path, not just the topic: the output lands as
  // "<name>/value", and models routinely read_series() what this returns.
  json series_paths = json::array();
  for (const auto& o : outputs) {
    series_paths.push_back(o + "/value");
  }
  json result = {
      {"created", name},
      {"series", outputs.size() == 1 ? json(outputs.front() + "/value") : series_paths},
      {"inputs", inputs}};
  // A join that survives but loses most of its rows is a legitimate surprise
  // worth naming: "of 20000 samples, 340 line up" is the difference between a
  // usable series and a handful of stray points, and nothing else would say so.
  if (forecast.checked && forecast.shared < forecast.smallest) {
    result["joined_points"] = forecast.shared;
    result["shortest_input_points"] = forecast.smallest;
  }
  result["verify_with"] = "read_series on " + outputs.front() + "/value";
  return ToolResult::success(result.dump());
}

// Raw form: the model authored the whole Luau rule; declare its inputs, pass
// the script through verbatim, and address the resulting marker set at
// `output` (a series path, or "__global__" for every plot of the dataset;
// defaults to the first input so markers land where the data is).
ToolResult createMarkersFromRule(const json& args, ToolContext& ctx) {
  const std::string rule = args["rule"].get<std::string>();
  if (rule.empty()) {
    return ToolResult::failure("create_markers 'rule' must be a non-empty Luau script");
  }
  if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty()) {
    return ToolResult::failure(
        "create_markers with 'rule' requires a non-empty 'inputs' array of topic/field paths "
        "(the series the rule reads via series(...))");
  }
  std::vector<std::string> inputs;
  for (const auto& in : args["inputs"]) {
    if (!in.is_string()) {
      return ToolResult::failure("'inputs' entries must be strings (topic/field paths)");
    }
    inputs.push_back(canonicalSeriesPath(in.get<std::string>()));
  }
  // Same resolution as create_derived_series, and for the same reason: the rule
  // looks its inputs up by exact name via series(...), so a path that is merely
  // close produces a rule that matches nothing and silently draws no markers.
  if (auto catalog = ctx.host.catalogSnapshot()) {
    for (auto& in : inputs) {
      auto lookup = resolveSeriesPath(*catalog, in);
      if (!lookup.resolved) {
        return ToolResult::failure(seriesLookupError(in, lookup));
      }
      in = lookup.resolved->path;
    }
  }
  const std::string output = args.contains("output") && args["output"].is_string()
                                 ? canonicalSeriesPath(args["output"].get<std::string>())
                                 : inputs.front();

  std::vector<std::string_view> input_views(inputs.begin(), inputs.end());
  auto topics = ctx.dp.createMarkers(
      "assistant_markers", PJ::Span<const std::string_view>(input_views.data(), input_views.size()), output, rule,
      "{}");
  if (!topics) {
    return ToolResult::failure("create_markers failed: " + topics.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  json result = {{"created_markers_on", output}, {"inputs", inputs}, {"form", "rule"}};
  // Hand back what was actually produced, not just what was asked for.
  if (json published = publishedMarkerSummary(ctx, *topics); !published.is_null()) {
    result.update(published);
  }
  return ToolResult::success(result.dump());
}

ToolResult createMarkers(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot create markers)");
  }

  // Two mutually exclusive forms (mirrors create_derived_series's
  // expression-XOR-body): a raw Luau `rule` over declared `inputs` — the model
  // writes the whole marker script against the vocabulary documented in the
  // tool schema — or the simple series/comparison/threshold template kept for
  // small models. On PJ4 main the host rejects kind="markers" and either form
  // returns that error to the model by design (graceful degrade).
  const bool has_rule = args.contains("rule") && args["rule"].is_string();
  const bool has_template = args.contains("series") || args.contains("threshold");
  if (has_rule && has_template) {
    return ToolResult::failure(
        "create_markers takes EITHER a raw 'rule' (with 'inputs') OR the series/comparison/threshold "
        "template — not both");
  }
  if (has_rule) {
    return createMarkersFromRule(args, ctx);
  }

  if (!args.contains("series") || !args["series"].is_string()) {
    return ToolResult::failure(
        "create_markers requires a string 'series' (a topic/field path), or a raw 'rule' with 'inputs'");
  }
  const std::string series = canonicalSeriesPath(args["series"].get<std::string>());
  const std::string comparison = args.value("comparison", std::string(">"));
  if (comparison != ">" && comparison != "<" && comparison != ">=" && comparison != "<=") {
    return ToolResult::failure("'comparison' must be one of >, <, >=, <=");
  }
  if (!args.contains("threshold") || !args["threshold"].is_number()) {
    return ToolResult::failure("create_markers requires a numeric 'threshold'");
  }
  const double threshold = args["threshold"].get<double>();
  // Locale-independent, round-trippable literal (std::to_string is locale
  // sensitive — a comma decimal separator breaks the generated Luau — and
  // truncates to 6 decimals, corrupting small thresholds).
  const std::string threshold_lit = json(threshold).dump();
  const std::string label = luaStringEscape(args.value("label", "exceeds " + threshold_lit));
  const std::string style = args.value("style", std::string("region"));
  if (style != "region" && style != "line") {
    return ToolResult::failure(
        "'style' must be \"region\" (one shaded band per exceedance stretch) or "
        "\"line\" (one vertical line per matching sample)");
  }

  const std::string opts = "{label=\"" + label + "\", severity=\"warning\"}";
  std::string rule = "local s = series(\"" + luaStringEscape(series) + "\")\n";
  if (style == "region") {
    // One region per contiguous stretch of matching samples: open on the first
    // matching sample, close on the first non-matching one (or at series end).
    rule += "local open = false\n";
    rule += "local last_t = nil\n";
    rule += "for i = 0, s:size() - 1 do\n";
    rule += "  local p = s:at(i)\n";
    rule += "  last_t = p.t\n";
    rule += "  local hit = p.v " + comparison + " " + threshold_lit + "\n";
    rule += "  if hit and not open then\n";
    rule += "    startMarker(p.t)\n";
    rule += "    open = true\n";
    rule += "  elseif open and not hit then\n";
    rule += "    closeMarker(p.t, " + opts + ")\n";
    rule += "    open = false\n";
    rule += "  end\n";
    rule += "end\n";
    rule += "if open and last_t then\n";
    rule += "  closeMarker(last_t, " + opts + ")\n";
    rule += "end\n";
  } else {
    rule += "for i = 0, s:size() - 1 do\n";
    rule += "  local p = s:at(i)\n";
    rule += "  if p.v " + comparison + " " + threshold_lit + " then\n";
    rule += "    createVerticalMarker(p.t, " + opts + ")\n";
    rule += "  end\n";
    rule += "end\n";
  }

  // The output marker topic addresses where the set renders: a persistent
  // kind="markers" node requires one (empty is preview-only). Use the input
  // series' own field path (markerSeriesKey semantics) so the markers appear
  // on every plot showing that series.
  std::array<std::string_view, 1> inputs{series};
  auto topics = ctx.dp.createMarkers(
      "assistant_markers", PJ::Span<const std::string_view>(inputs.data(), inputs.size()),
      /*output_marker_topic=*/series, rule, "{}");
  if (!topics) {
    return ToolResult::failure("create_markers failed: " + topics.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  json result = {{"created_markers_on", series}, {"style", style}, {"rule", label}};
  if (json published = publishedMarkerSummary(ctx, *topics); !published.is_null()) {
    result.update(published);
  }
  return ToolResult::success(result.dump());
}

ToolResult removeMarkers(const json& /*args*/, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot remove markers)");
  }
  auto status = ctx.dp.remove("assistant_markers");
  if (!status) {
    return ToolResult::failure("remove_markers failed: " + status.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  return ToolResult::success(json({{"removed", "assistant_markers"}}).dump());
}

// What this assistant has installed in the session so far.
//
// dp.list() enumerates only THIS plugin's nodes, so both this and the removal
// below are bounded by construction: neither can see or touch loaded data, or
// anything another plugin created.
ToolResult listCreated(const json& /*args*/, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1");
  }
  auto ids = ctx.dp.list();
  if (!ids) {
    return ToolResult::failure("list failed: " + ids.error());
  }
  json arr = json::array();
  for (const auto& id : *ids) {
    arr.push_back(id);
  }
  return ToolResult::success(json({{"created", arr}, {"count", arr.size()}}).dump());
}

ToolResult removeDerivedSeries(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1");
  }
  if (!args.contains("name") || !args["name"].is_string() || args["name"].get<std::string>().empty()) {
    return ToolResult::failure("remove_derived_series requires a non-empty string 'name'");
  }
  const std::string name = args["name"].get<std::string>();
  // Check membership first so an unknown name gets a useful answer listing what
  // does exist, rather than whatever the host says about a handle it never had.
  auto ids = ctx.dp.list();
  if (ids && std::find(ids->begin(), ids->end(), name) == ids->end()) {
    std::string known;
    for (const auto& id : *ids) {
      known += (known.empty() ? "" : ", ") + id;
    }
    return ToolResult::failure(
        "this assistant did not create '" + name + "'" +
        (known.empty() ? "; it has created nothing in this session" : "; it has created: " + known) +
        ". Only series created here can be removed — loaded data cannot be touched.");
  }
  auto status = ctx.dp.remove(name);
  if (!status) {
    return ToolResult::failure("remove_derived_series failed: " + status.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  return ToolResult::success(json({{"removed", name}}).dump());
}

ToolResult reportStatus(const json& /*args*/, ToolContext& ctx) {
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  // Name the datasets rather than only counting them: "2" cannot be acted on,
  // and which two is exactly what decides whether series may be combined.
  json names = json::array();
  for (const auto& src : catalog->dataSources()) {
    names.push_back(std::string(PJ::sdk::toStringView(src.name)));
  }
  json out = {
      {"data_sources", catalog->dataSources().size()},
      {"topics", catalog->topics().size()},
      {"fields", catalog->fields().size()}};
  if (!names.empty()) {
    out["dataset_names"] = names;
  }
  return ToolResult::success(out.dump());
}

}  // namespace

// --- catalog digest --------------------------------------------------------

std::string catalogDigest(const PJ::sdk::ToolboxHostView& host, std::size_t budget_chars) {
  auto catalog = host.catalogSnapshot();
  if (!catalog) {
    return "Loaded data: unavailable (" + catalog.error() + "). Use list_topics to look it up.";
  }
  auto topics = catalog->topics();
  auto fields = catalog->fields();
  if (topics.empty()) {
    return "Loaded data: nothing is loaded yet.";
  }

  // Which dataset each topic belongs to. PJ4 can hold several loaded at once —
  // two runs of the same robot, say — and a flat topic list hides that
  // completely: the model cannot offer to compare them because it does not know
  // there are two, and cannot avoid mixing them for the same reason. Topics are
  // grouped contiguously per source (first_topic/topic_count), so this is a
  // lookup table rather than a scan. Empty when the host reports no sources, in
  // which case the listing stays exactly as it was.
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(*catalog);

  // Pass 1: the full tree, "topic: fieldA (type), fieldB (type)". Preferred,
  // because it gives the model complete paths and types with no follow-up.
  auto build = [&](bool with_fields, std::size_t& shown) -> std::string {
    std::string body;
    shown = 0;
    std::string current_dataset;
    for (std::uint32_t ti = 0; ti < topics.size(); ++ti) {
      const auto& topic = topics[ti];
      std::string dataset_header;
      if (auto it = topic_dataset.find(ti); it != topic_dataset.end() && it->second != current_dataset) {
        current_dataset = it->second;
        dataset_header = "dataset \"" + current_dataset + "\":\n";
      }
      const auto topic_name = PJ::sdk::toStringView(topic.name);
      std::string line = dataset_header + "  " + std::string(topic_name);
      if (with_fields) {
        line += ": ";
        for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
          const std::size_t idx = topic.first_field + fi;
          if (idx >= fields.size()) {
            break;
          }
          if (fi != 0) {
            line += ", ";
          }
          line += std::string(PJ::sdk::toStringView(fields[idx].name));
          line += " (";
          line += primitiveTypeName(PJ::sdk::fromAbiType(fields[idx].type));
          line += ")";
        }
      }
      line += "\n";
      if (body.size() + line.size() > budget_chars) {
        break;
      }
      body += line;
      ++shown;
    }
    return body;
  };

  std::size_t shown = 0;
  std::string body = build(/*with_fields=*/true, shown);
  bool with_fields = true;
  if (shown < topics.size()) {
    // Didn't fit with fields — retry with topic names only, which is far
    // cheaper per topic and usually fits a dataset that the full tree can't.
    std::size_t shown_names = 0;
    std::string names_body = build(/*with_fields=*/false, shown_names);
    if (shown_names > shown) {
      body = std::move(names_body);
      shown = shown_names;
      with_fields = false;
    }
  }

  std::string header = "Loaded data (" + std::to_string(topics.size()) + " topic(s)";
  if (shown < topics.size()) {
    header += ", listing the first " + std::to_string(shown);
  }
  header += "):\n";

  std::string footer;
  if (!with_fields) {
    footer += "Field names are omitted here — call describe_topic for a topic's fields.\n";
  }
  if (shown < topics.size()) {
    footer +=
        "This listing is TRUNCATED; topics not shown above still exist. Use list_topics with a filter to find "
        "them.\n";
  }
  return header + body + footer;
}

// --- registry --------------------------------------------------------------

void ToolRegistry::add(ToolSpec spec) {
  tools_.push_back(std::move(spec));
}

ToolRegistry::ToolRegistry() {
  using nlohmann::json;
  const json empty_obj = {{"type", "object"}, {"properties", json::object()}};

  add(
      {"list_topics",
       "Search the loaded topics by substring. You do NOT need this to find out what is loaded — "
       "that listing already arrives with every message. Call it only to look for something the "
       "listing does not show, e.g. when it was truncated or the data changed. Optional 'filter' "
       "and 'limit'.",
       {{"type", "object"},
        {"properties",
         {{"filter", {{"type", "string"}, {"description", "case-sensitive substring to match topic names"}}},
          {"dataset",
           {{"type", "string"},
            {"description", "only topics from this dataset (substring); useful when several are loaded"}}},
          {"limit", {{"type", "integer"}, {"description", "max topics to return (default 100, capped at 500)"}}}}}},
       &listTopics});

  add(
      {"describe_topic",
       "List the fields of one topic, with types and full paths. Only needed for a topic whose "
       "fields are NOT already in the listing you were given — do not use it to confirm a path you "
       "can already see.",
       {{"type", "object"},
        {"properties", {{"topic", {{"type", "string"}, {"description", "exact topic name"}}}}},
        {"required", json::array({"topic"})}},
       &describeTopic});

  add(
      {"read_series",
       "Read summary statistics ('stats') or a min/max-preserving downsample ('buckets'). Never "
       "returns raw samples. Bucket times 't' are seconds relative to the series start.\n"
       "'paths' is an ARRAY — ask for every series you want stats for in ONE call. Each call is a "
       "round trip that re-sends the whole conversation, so twelve one-by-one cost twelve times "
       "twelve together. A bad path returns as an error beside the results that worked.\n"
       "mode='buckets' reads ONE series: several shapes in one response would be coarsened past "
       "usefulness. Batch the stats, then ask for the shape of whichever mattered.",
       {{"type", "object"},
        {"properties",
         {{"paths",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "topic/field paths; a bare string is accepted for a single series"}}},
          {"mode", {{"type", "string"}, {"enum", json::array({"stats", "buckets"})}}},
          {"max_points", {{"type", "integer"}, {"description", "bucket count for mode=buckets (<=500)"}}}}},
        {"required", json::array({"paths"})}},
       &readSeriesTool});

  add(
      {"create_derived_series",
       "Create a new derived timeseries computed live from one or more inputs. Each sample sees "
       "'value' (first input), 'v1'..'vN' (further inputs, in order), and 'time' (seconds). Use "
       "'expression' for a stateless formula (e.g. 'value * 2'); for stateful transforms like a "
       "derivative, use 'global' (runs once, persistent locals) + 'body' (statements ending in "
       "return; return nothing to suppress a sample).",
       {{"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "name of the new series"}}},
          {"inputs", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "topic/field paths"}}},
          {"outputs",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "OPTIONAL — several series from ONE node (default: just 'name'). The body then returns one value per "
             "output, in order: outputs ['roll','pitch','yaw'] with a body ending 'return r, p, y'. Prefer this over "
             "three nodes recomputing the same intermediate."}}},
          {"expression", {{"type", "string"}, {"description", "stateless Luau expression over value/v1../time"}}},
          {"body",
           {{"type", "string"}, {"description", "full Luau statements ending in return (overrides expression)"}}},
          {"global",
           {{"type", "string"}, {"description", "Luau run once per instance; locals persist across samples"}}}}},
        {"required", json::array({"name", "inputs"})}},
       &createDerivedSeries});

  add(
      {"create_markers",
       "Create plot markers from a Luau rule you write (preferred), or from a simple threshold "
       "template. There is ONE assistant marker set: calling this again REPLACES it (use "
       "remove_markers to clear it).\n"
       "CHOOSE THE SHAPE, it is not always a line: a condition true over a STRETCH of time wants ONE "
       "region per stretch (startMarker/closeMarker), never one line per matching sample — zoomed out "
       "those merge into a solid wall that hides the data underneath. This call reports how many "
       "markers it produced and of which kind: past ~50, the shape is wrong, so merge contiguous hits "
       "into regions or raise the threshold.\n"
       "THRESHOLDS on raw high-rate signals (IMU and the like, tens of Hz and up): 'a sample "
       "crosses X' marks vibration, not events — across thousands of samples a few sigma from the "
       "mean is a routine excursion, not an outlier. Require the condition to hold for a minimum "
       "duration (or smooth the signal first), and cross-check the events against an independent "
       "slower signal before you label them.\n"
       "RAW FORM: pass 'inputs' (series the rule reads) + 'rule' (a whole Luau script, run once "
       "over the full series) + optional 'output' (series path the markers attach to, or "
       "\"__global__\" for all plots; default = first input). Use \"__global__\" only when the "
       "events mean something against ANY signal (stationary periods, dropouts, mode changes); "
       "for events tied to one signal keep the default, or the markers become noise on every "
       "plot in the layout that has nothing to do with the rule.\n"
       "Rule vocabulary:\n"
       "- series(\"topic/field\") -> accessor or nil: s:size(); s:at(i) 0-based -> {t, v} or nil; "
       "s:atTime(t) -> interpolated value. GetSeriesNames() -> declared input names. "
       "Timestamps t are int64 NANOSECONDS.\n"
       "- startMarker(t) then closeMarker(t2, opts) -> ONE shaded region per pair.\n"
       "- createVerticalMarker(t, opts); createPointMarker(t, y, opts); "
       "createHorizontalMarker(y, opts); createBandMarker(y_low, y_high, opts); "
       "createMarker(x?, y?, opts).\n"
       "- bandPower(s, f_lo_hz, f_hi_hz) -> summed FFT power for spectral rules.\n"
       "- opts (all optional): label, description, category, color \"#RRGGBB\", severity "
       "(\"info\"|\"warning\"|\"error\"|\"critical\"), status.\n"
       "TEMPLATE FORM (simple threshold): 'series' + 'comparison' (>, <, >=, <=) + 'threshold' + "
       "optional 'label' and 'style' (\"region\" default: one band per exceedance stretch; "
       "\"line\": one vertical line per matching sample).",
       {{"type", "object"},
        {"properties",
         {{"rule", {{"type", "string"}, {"description", "raw Luau marker script (raw form)"}}},
          {"inputs",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "topic/field paths the rule reads (raw form)"}}},
          {"output",
           {{"type", "string"},
            {"description",
             "series path the marker set renders on, or \"__global__\" (raw form, "
             "optional; default = first input)"}}},
          {"series", {{"type", "string"}, {"description", "topic/field path (template form)"}}},
          {"comparison", {{"type", "string"}, {"enum", json::array({">", "<", ">=", "<="})}}},
          {"threshold", {{"type", "number"}}},
          {"style", {{"type", "string"}, {"enum", json::array({"region", "line"})}}},
          {"label", {{"type", "string"}, {"description", "marker label (optional)"}}}}}},
       &createMarkers});

  add(
      {"remove_markers",
       "Remove the assistant-created marker set from all plots. Only affects markers this "
       "assistant created; cannot delete user data.",
       empty_obj, &removeMarkers});

  add(
      {"list_created",
       "List the derived series and marker sets THIS assistant has created in this session. Use it "
       "before creating something that may already exist, or to find the name to remove.",
       empty_obj, &listCreated});

  add(
      {"remove_derived_series",
       "Delete a derived series this assistant created, by its name. Only its own creations — loaded "
       "data cannot be touched. Use it to withdraw a series that turned out wrong instead of leaving "
       "it in the user's panel.",
       {{"type", "object"},
        {"properties", {{"name", {{"type", "string"}, {"description", "name given at creation"}}}}},
        {"required", json::array({"name"})}},
       &removeDerivedSeries});

  add(
      {"report_status",
       "Counts of loaded sources/topics/fields. Only when the user asks for an overview — never as "
       "an opening step, since the listing you already have covers it.",
       empty_obj, &reportStatus});
}

const ToolSpec* ToolRegistry::find(std::string_view name) const {
  for (const auto& spec : tools_) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

ToolResult ToolRegistry::execute(std::string_view name, const nlohmann::json& args, ToolContext& ctx) const {
  const ToolSpec* spec = find(name);
  if (spec == nullptr) {
    return ToolResult::failure("unknown tool '" + std::string(name) + "'");
  }
  // Choke point for the no-throw contract: executors use nlohmann typed getters
  // on MODEL-controlled arguments, and a wrong-typed value throws. The call
  // chain above this (GuiExecutor -> onTick -> plugin vtable) must never see an
  // exception, so convert anything thrown into a failure the model can react to.
  try {
    return spec->executor(args.is_object() ? args : nlohmann::json::object(), ctx);
  } catch (const std::exception& e) {
    return ToolResult::failure(std::string(name) + ": invalid arguments: " + e.what());
  }
}

nlohmann::json ToolRegistry::toOllamaTools() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& spec : tools_) {
    arr.push_back(
        {{"type", "function"},
         {"function", {{"name", spec.name}, {"description", spec.description}, {"parameters", spec.input_schema}}}});
  }
  return arr;
}

nlohmann::json ToolRegistry::toMcpToolsList() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& spec : tools_) {
    arr.push_back({{"name", spec.name}, {"description", spec.description}, {"inputSchema", spec.input_schema}});
  }
  return arr;
}

}  // namespace assistant_agent
