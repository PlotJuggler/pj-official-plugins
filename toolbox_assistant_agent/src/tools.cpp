// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The assistant's tool executors. Each is a pure function of (args, host views)
// and returns a ToolResult whose `content` the model reads. Every failure is
// returned as data (ok=false) — exceptions must never cross the plugin ABI, so
// nothing here throws out.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <span>
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
  std::vector<std::pair<PJ::Timestamp, PJ::Timestamp>> regions;
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
      if (m.kind == PJ::sdk::MarkerKind::kRegion && m.t_end > m.t_start) {
        regions.emplace_back(m.t_start, m.t_end);
      }
    }
  }
  json out = {{"markers_created", total}};
  if (!by_kind.empty()) {
    out["by_kind"] = by_kind;
  }
  // `covered_s` is the UNION of the region intervals — the log time the regions
  // actually cover, which is what a model quotes back to the user. The envelope
  // (first start to last end) used to be reported here as `span_s`, and models
  // read it as coverage every single time: two regions covering 95.1 s were
  // announced as "~122 s, half the drive" because the slow stretch between them
  // sat inside the envelope. Zero-duration marker kinds contribute nothing, and
  // a set with no regions omits the field rather than reporting a meaningless 0.
  if (!regions.empty()) {
    std::sort(regions.begin(), regions.end());
    PJ::Timestamp covered_ns = 0;
    PJ::Timestamp cur_start = regions.front().first;
    PJ::Timestamp cur_end = regions.front().second;
    for (std::size_t i = 1; i < regions.size(); ++i) {
      if (regions[i].first > cur_end) {
        covered_ns += cur_end - cur_start;
        cur_start = regions[i].first;
        cur_end = regions[i].second;
      } else {
        cur_end = std::max(cur_end, regions[i].second);
      }
    }
    covered_ns += cur_end - cur_start;
    out["covered_s"] = static_cast<double>(covered_ns) * 1e-9;
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

// ResolvedSeries/SeriesLookup live in tool_registry.hpp: resolution is where
// the multi-dataset rules live, and the tests drive it directly.

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

// Cap on how many near misses we name. The error text is fed back to the model
// and then re-sent on every later round-trip of the turn, so an unbounded list
// would be paid for repeatedly.
constexpr std::size_t kMaxCandidates = 10;

}  // namespace

// Resolve one "topic/field" curve path (joinSeriesPath convention, the same the
// rest of PJ4 uses) by scanning the catalog — models call read_series
// repeatedly per turn, so avoid materializing a full path index.
//
// An exact match wins over abbreviations. An abbreviated path resolves only
// when exactly one series matches: with several the answer is the candidate
// list, never a guess, because silently picking one would attach a transform to
// the wrong signal and look like it worked.
//
// Datasets: the path may carry the host's qualifier convention,
// "dataset:topic/field". The qualifier is matched against the KNOWN source
// names (longest match wins) rather than parsed at ':', so a name like
// "[stream] UDP Server" needs no escaping. An unqualified path whose exact
// topic/field exists in SEVERAL datasets is refused as ambiguous — two runs of
// the same robot share every topic name, and silently taking the first-loaded
// one reads (or worse, installs onto) whichever file happened to load first.
// With several sources loaded, every path this returns is in qualified form, so
// results disclose which dataset they came from and candidates can be copied
// back verbatim.
SeriesLookup resolveSeriesPath(const PJ::sdk::CatalogSnapshot& catalog, const std::string& series) {
  SeriesLookup out;
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const auto sources = catalog.dataSources();

  std::string bare = series;
  std::uint32_t topic_lo = 0;
  auto topic_hi = static_cast<std::uint32_t>(topics.size());
  std::size_t qualifier_len = 0;
  for (const auto& src : sources) {
    const std::string name(PJ::sdk::toStringView(src.name));
    if (name.empty() || name.size() <= qualifier_len || series.size() <= name.size() || series[name.size()] != ':' ||
        series.compare(0, name.size(), name) != 0) {
      continue;
    }
    qualifier_len = name.size();
    topic_lo = src.first_topic;
    topic_hi = std::min(src.first_topic + src.topic_count, static_cast<std::uint32_t>(topics.size()));
  }
  if (qualifier_len != 0) {
    bare = series.substr(qualifier_len + 1);
  }

  const auto want = pathSegments(bare);
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(catalog);
  auto dataset_of = [&](std::uint32_t ti) {
    const auto it = topic_dataset.find(ti);
    return it == topic_dataset.end() ? std::string() : it->second;
  };
  auto qualified = [&](std::uint32_t ti, const std::string& full) {
    const std::string dataset = dataset_of(ti);
    return dataset.empty() ? full : dataset + ":" + full;
  };

  std::optional<ResolvedSeries> exact;
  bool exact_ambiguous = false;
  std::vector<std::string> exact_candidates;
  std::optional<ResolvedSeries> fuzzy;
  for (std::uint32_t ti = topic_lo; ti < topic_hi; ++ti) {
    const auto& topic = topics[ti];
    const auto topic_name = PJ::sdk::toStringView(topic.name);
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      const std::string full = joinSeriesPath(topic_name, PJ::sdk::toStringView(fields[idx].name));
      if (full == bare) {
        if (!exact) {
          exact =
              ResolvedSeries{fields[idx].handle, std::string(topic_name), qualified(ti, full), full, dataset_of(ti)};
        } else {
          exact_ambiguous = true;
        }
        if (exact_candidates.size() < kMaxCandidates) {
          exact_candidates.push_back(qualified(ti, full));
        }
        continue;
      }
      if (segmentsContain(pathSegments(full), want)) {
        if (out.candidates.size() < kMaxCandidates) {
          out.candidates.push_back(qualified(ti, full));
        }
        if (!fuzzy) {
          fuzzy =
              ResolvedSeries{fields[idx].handle, std::string(topic_name), qualified(ti, full), full, dataset_of(ti)};
        } else {
          out.ambiguous = true;
        }
      }
    }
  }
  if (exact) {
    if (exact_ambiguous) {
      out.ambiguous = true;
      out.candidates = std::move(exact_candidates);
      return out;
    }
    out.resolved = std::move(exact);
    out.candidates.clear();
    out.ambiguous = false;
    return out;
  }
  if (!out.ambiguous && fuzzy) {
    out.resolved = std::move(fuzzy);
    out.candidates.clear();
  }
  return out;
}

namespace {

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

// Whether the HOST can address this series by name for a create, and the error
// to hand back when it cannot. The create side of pj.data_processors.v1 takes
// bare names; when the same topic/field exists in several datasets the bare
// name no longer picks one, so handing it over would either fail or land on
// whichever dataset the host resolves first. Reads are unaffected — they go by
// handle — so this is a create-only limit, refused loudly here instead of
// silently mistargeted there.
std::optional<std::string> hostCreateBlocker(const PJ::sdk::CatalogSnapshot& catalog, const ResolvedSeries& resolved) {
  const auto bare = resolveSeriesPath(catalog, resolved.host_path);
  if (!bare.ambiguous) {
    return std::nullopt;
  }
  return "cannot create from '" + resolved.path + "': several loaded datasets share the path '" + resolved.host_path +
         "', and the host's create interface addresses inputs by bare name, so it cannot target that specific "
         "dataset. Reading it works (read_series); to build on it, ask the user to keep only the relevant file "
         "loaded.";
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
  json out = {{"count", s.count}, {"duration_s", s.duration_s}, {"rate_hz", s.rate_hz}};
  // NaN/inf poison a straight sum, so min/max/mean/stddev are computed over
  // finite values only; when none exist there is nothing honest to report, so
  // they are omitted rather than serialized as NaN (which nlohmann turns into
  // `null` — indistinguishable from a field that was never populated).
  if (s.has_values) {
    out["min"] = s.min;
    out["max"] = s.max;
    out["min_at_s"] = s.min_at_s;
    out["max_at_s"] = s.max_at_s;
    out["mean"] = s.mean;
    out["stddev"] = s.stddev;
  } else {
    out["note"] = "no finite values";
  }
  // The spacing facts that count/mean/rate cannot carry: a dropout leaves all
  // three untouched. The keys are self-describing on purpose — the tool's
  // schema description says nothing about them, so they cost prefix tokens in
  // no turn and appear exactly when a series is read.
  if (s.has_gap) {
    out["max_gap_s"] = s.max_gap_s;
    out["max_gap_at_s"] = s.max_gap_at_s;
  }
  if (s.invalid > 0) {
    out["invalid"] = s.invalid;
    out["invalid_fraction"] = static_cast<double>(s.invalid) / static_cast<double>(s.count);
  }
  return out;
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

// Buckets are the expensive, variable payload — several of them share ONE
// response cap (see batchBuckets), so the batch stays smaller than the flat
// per-path cap above: past this many series sharing the cap coarsens every
// one of them past usefulness rather than saving a round trip.
constexpr std::size_t kMaxBucketsBatchPaths = 8;

// Collect the requested paths from either a bare string or an array.
//
// The parameter is named `paths` rather than `series` on purpose. The object
// store holds point clouds, images and occupancy grids behind the same read
// view, and when this call learns to summarize those too, "series" would be the
// wrong word for what it takes. The model learns the name from the description,
// so renaming it later is more expensive than choosing it now. `series` still
// works, undocumented, so a conversation already in flight does not break.
std::vector<std::string> requestedPaths(
    const json& args, std::initializer_list<const char*> keys = {"paths", "series"}) {
  std::vector<std::string> out;
  for (const char* key : keys) {
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
  std::string path;     // the resolved path, which may differ from what was asked
  std::string topic;    // the owning topic — the key display-time conversion wants
  std::string dataset;  // source name; empty when at most one dataset is loaded
  std::vector<std::int64_t> ts;
  std::vector<double> vals;
  // Present only when a t_start_s/t_end_s window was requested and applied —
  // echoed back on the entry so a later turn knows count/rate_hz/duration_s
  // and the bucket time axis are the WINDOW's, not the whole series'.
  json window;
  // Computed by readOne on the full, unwindowed ts/vals — see
  // FlatRunSummary's comment for why this must stay whole-series even when
  // a window narrows everything else in this struct.
  FlatRunSummary flat;
};

// With a playback host bound, report where this series STARTS on the plot
// axis, so the model can turn bucket-relative times into seek/zoom targets:
// display time of a bucket = t_start_display_s + bucket.t. Best-effort — the
// conversion is frame-dependent (user-editable offsets), never an error here.
json statsWithDisplayStart(
    const SeriesStats& stats, ToolContext& ctx, const std::string& topic, const FlatRunSummary& flat) {
  json stats_json = statsToJson(stats);
  if (ctx.playback.valid() && stats.count > 0) {
    if (auto display_s = ctx.playback.toDisplayTime(topic, stats.t_start_ns)) {
      stats_json["t_start_display_s"] = *display_s;
    }
  }
  // Whole-series facts (see FlatRunSummary): a constant channel says so
  // instead of the flat span, which would just restate "the whole thing".
  if (flat.constant) {
    stats_json["constant"] = true;
  } else if (flat.has_flat_span) {
    stats_json["flat_span_s"] = flat.flat_span_s;
    stats_json["flat_span_at_s"] = flat.flat_span_at_s;
  }
  return stats_json;
}

SeriesRead readOne(const PJ::sdk::CatalogSnapshot& catalog, ToolContext& ctx, const std::string& want) {
  SeriesRead r;
  r.path = want;
  auto lookup = resolveSeriesPath(catalog, want);
  if (!lookup.resolved) {
    r.error = seriesLookupError(want, lookup);
    return r;
  }
  r.path = lookup.resolved->path;
  r.topic = lookup.resolved->topic;
  r.dataset = lookup.resolved->dataset;
  auto view = ctx.host.readSeries(lookup.resolved->handle);
  if (!view) {
    r.error = "read failed for '" + want + "': " + view.error();
    return r;
  }
  if (!readSeriesDoubles(*view, r.ts, r.vals)) {
    r.error = "series '" + want + "' is not a numeric time series";
    return r;
  }
  // Whole-series facts, computed here — before any t_start_s/t_end_s window
  // narrows r.ts/r.vals below — so "never changes" / "freezes for N s" stay
  // claims about the entire recording even under a windowed read.
  r.flat = flatRunSummary(r.ts, r.vals);
  r.ok = true;
  return r;
}

// The DataSourceHandle a resolved series' dataset qualifies to, for
// toDisplayTimeForSource — which needs a handle, while ResolvedSeries only
// carries the dataset's NAME (empty when at most one dataset is loaded, see
// tool_registry.hpp). Matches the name against the catalog's data sources; an
// empty name with exactly one loaded source is that source, unambiguously.
std::optional<PJ::sdk::DataSourceHandle> dataSourceHandleFor(
    const PJ::sdk::CatalogSnapshot& catalog, const std::string& dataset_name) {
  const auto sources = catalog.dataSources();
  if (!dataset_name.empty()) {
    for (const auto& src : sources) {
      if (PJ::sdk::toStringView(src.name) == dataset_name) {
        return src.handle;
      }
    }
    return std::nullopt;
  }
  if (sources.size() == 1) {
    return sources.front().handle;
  }
  return std::nullopt;
}

// Best-effort absolute-ns -> display-seconds conversion for one series,
// preferring the dataset-scoped host call (immune to the "ambiguous topic
// between datasets" failure mode the plain topic-scoped one has) and falling
// back to the topic-scoped one when the host does not offer the former.
// Empty when neither is bound or neither succeeds — never guessed.
std::optional<double> toDisplaySeconds(
    ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const SeriesRead& r, std::int64_t absolute_ns) {
  if (auto handle = dataSourceHandleFor(catalog, r.dataset)) {
    if (auto display_s = ctx.playback.toDisplayTimeForSource(*handle, absolute_ns)) {
      return *display_s;
    }
  }
  if (auto display_s = ctx.playback.toDisplayTime(r.topic, absolute_ns)) {
    return *display_s;
  }
  return std::nullopt;
}

// Narrows a successfully-read series to [t_start_s, t_end_s] on the DISPLAY
// axis (playback's, plot_tab zoom's, stats' own t_start_display_s) when
// either bound was requested; a no-op otherwise. The series' own first
// sample is the only anchor available for the ns<->display conversion (see
// toDisplaySeconds): the affine offset it yields — display(ts.front()) minus
// ts.front() in seconds — is applied to translate the window's bounds into
// absolute ns (floor at the start, ceil at the end), and the series is
// trimmed to that ns range via lower_bound/upper_bound before stats or
// buckets ever see it. So a windowed read's t_start_display_s and bucket 't'
// come out relative to the first sample INSIDE the window, never to
// t_start_s itself.
//
// Failure — no anchor sample, no conversion available, or an empty result —
// turns the entry into an error (r.ok becomes false) rather than silently
// reinterpreting the window as relative ns, which would quietly answer a
// different question than the one asked.
void applyDisplayWindow(
    SeriesRead& r, ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const std::optional<double>& t_start_s,
    const std::optional<double>& t_end_s) {
  if (!t_start_s && !t_end_s) {
    return;
  }
  if (r.ts.empty()) {
    r.ok = false;
    r.error = "cannot map the display window for this series";
    return;
  }
  const std::int64_t anchor_ns = r.ts.front();
  const std::optional<double> anchor_display = toDisplaySeconds(ctx, catalog, r, anchor_ns);
  if (!anchor_display) {
    r.ok = false;
    r.error = "cannot map the display window for this series";
    return;
  }
  const double offset = *anchor_display - static_cast<double>(anchor_ns) * 1e-9;
  auto toAbsNs = [&](double display_s, bool round_up) {
    const double ns = (display_s - offset) * 1e9;
    return static_cast<std::int64_t>(round_up ? std::ceil(ns) : std::floor(ns));
  };

  auto lo_it = r.ts.begin();
  auto hi_it = r.ts.end();
  if (t_start_s) {
    lo_it = std::lower_bound(r.ts.begin(), r.ts.end(), toAbsNs(*t_start_s, /*round_up=*/false));
  }
  if (t_end_s) {
    hi_it = std::upper_bound(r.ts.begin(), r.ts.end(), toAbsNs(*t_end_s, /*round_up=*/true));
  }
  if (lo_it >= hi_it) {
    const double series_start_display = *anchor_display;
    const double series_end_display = static_cast<double>(r.ts.back()) * 1e-9 + offset;
    std::ostringstream msg;
    msg << "no samples in window; series spans display [" << series_start_display << ", " << series_end_display << "]";
    r.ok = false;
    r.error = msg.str();
    return;
  }

  // Trim by INDEX, not by copying through the iterators above: ts and vals
  // are reassigned in place below, and an iterator pair that aliases the
  // vector being assigned is not safe to feed straight to vector::assign.
  const auto lo = static_cast<std::size_t>(lo_it - r.ts.begin());
  const auto hi = static_cast<std::size_t>(hi_it - r.ts.begin());
  std::vector<std::int64_t> windowed_ts(
      r.ts.begin() + static_cast<std::ptrdiff_t>(lo), r.ts.begin() + static_cast<std::ptrdiff_t>(hi));
  std::vector<double> windowed_vals(
      r.vals.begin() + static_cast<std::ptrdiff_t>(lo), r.vals.begin() + static_cast<std::ptrdiff_t>(hi));
  r.ts = std::move(windowed_ts);
  r.vals = std::move(windowed_vals);

  r.window = {{"axis", "display"}};
  if (t_start_s) {
    r.window["t_start_s"] = *t_start_s;
  }
  if (t_end_s) {
    r.window["t_end_s"] = *t_end_s;
  }
}

// Bucketize and render as the JSON array both readSeriesTool's 'buckets' mode
// (single-series and batched) and evaluate's optional bucket summary send
// back: one object per bucket, min/max/mean only when it holds a finite
// sample, an 'invalid' count only when it holds a non-finite one.
json renderBucketArray(std::span<const std::int64_t> ts, std::span<const double> vals, std::size_t max_points) {
  auto buckets = bucketize(ts, vals, max_points);
  json bucket_arr = json::array();
  for (const auto& b : buckets) {
    json entry = {{"t", b.t_rel_s}, {"n", b.count}};
    if (b.count > 0) {
      entry["min"] = b.min;
      entry["max"] = b.max;
      entry["mean"] = b.mean;
    }
    if (b.invalid > 0) {
      entry["invalid"] = b.invalid;
    }
    bucket_arr.push_back(entry);
  }
  return bucket_arr;
}

// Coarsen until the serialized payload fits the response cap so spiky data
// stays representable without overrunning the model's context. Returns
// `base` with a "buckets" array attached (and a "note" when it still does not
// fit at the smallest allowed resolution). Shared by read_series's single-path
// 'buckets' mode and evaluate's optional bucket summary.
json withCoarsenedBuckets(
    json base, std::span<const std::int64_t> ts, std::span<const double> vals, std::size_t max_points) {
  for (;;) {
    base["buckets"] = renderBucketArray(ts, vals, max_points);
    std::string dumped = base.dump();
    if (dumped.size() <= kMaxResponseBytes || max_points <= 16) {
      if (dumped.size() > kMaxResponseBytes) {
        base["note"] = "coarsened to fit the response cap";
      }
      return base;
    }
    max_points /= 2;
  }
}

// Several series' buckets, sharing ONE response cap instead of each getting
// the single-series kMaxResponseBytes on its own: min(N, 4) times that cap,
// halved in lockstep across every series until the whole envelope fits (or
// each is down to 16 buckets — the same floor withCoarsenedBuckets uses). A
// bad path becomes an {"error"} entry beside the ones that worked, same as
// 'stats'. Stats do not depend on max_points, so they are computed once,
// outside the halving loop that only re-renders the bucket arrays.
json batchBuckets(ToolContext& ctx, const std::vector<SeriesRead>& reads, std::size_t max_points) {
  const std::size_t n = reads.size();
  const std::size_t cap = std::min(n, std::size_t{4}) * kMaxResponseBytes;

  std::vector<json> stats(n);
  std::size_t failed = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (reads[i].ok) {
      stats[i] = statsWithDisplayStart(computeStats(reads[i].ts, reads[i].vals), ctx, reads[i].topic, reads[i].flat);
    } else {
      ++failed;
    }
  }

  for (;;) {
    json read_arr = json::array();
    for (std::size_t i = 0; i < n; ++i) {
      const SeriesRead& r = reads[i];
      if (!r.ok) {
        read_arr.push_back({{"series", r.path}, {"error", r.error}});
        continue;
      }
      json entry = {{"series", r.path}, {"stats", stats[i]}, {"buckets", renderBucketArray(r.ts, r.vals, max_points)}};
      if (!r.window.is_null()) {
        entry["window"] = r.window;
      }
      read_arr.push_back(std::move(entry));
    }
    json out = {{"count", n}, {"read", read_arr}};
    if (failed != 0) {
      out["failed"] = failed;
    }
    std::string dumped = out.dump();
    if (dumped.size() <= cap || max_points <= 16) {
      if (dumped.size() > cap) {
        out["note"] = "coarsened to fit the response cap";
      }
      return out;
    }
    max_points /= 2;
  }
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

  // Optional display-axis window, shared by every path in this call (not
  // per-path — one read_series call reads one moment in time). Validated
  // once here so a backwards range fails the whole call instead of quietly
  // producing an empty-window error on every entry.
  std::optional<double> t_start_s;
  std::optional<double> t_end_s;
  if (args.contains("t_start_s") && !args["t_start_s"].is_null()) {
    t_start_s = args["t_start_s"].get<double>();
  }
  if (args.contains("t_end_s") && !args["t_end_s"].is_null()) {
    t_end_s = args["t_end_s"].get<double>();
  }
  if (t_start_s && t_end_s && *t_end_s <= *t_start_s) {
    return ToolResult::failure("t_end_s must be greater than t_start_s");
  }

  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }

  // readOne, then narrow to the window when one was requested — shared by
  // every mode below so 'stats' and 'buckets', single-path and batched, all
  // apply the same window the same way.
  auto readWindowed = [&](const std::string& want) {
    SeriesRead r = readOne(*catalog, ctx, want);
    if (r.ok) {
      applyDisplayWindow(r, ctx, *catalog, t_start_s, t_end_s);
    }
    return r;
  };

  if (mode == "stats") {
    // One entry per requested path, each carrying its own error. A single typo
    // must not cost the whole round trip — which is the entire point of asking
    // for several at once.
    json arr = json::array();
    std::size_t failed = 0;
    for (const auto& want : paths) {
      SeriesRead r = readWindowed(want);
      if (!r.ok) {
        ++failed;
        arr.push_back({{"series", r.path}, {"error", r.error}});
        continue;
      }
      json entry = {
          {"series", r.path}, {"stats", statsWithDisplayStart(computeStats(r.ts, r.vals), ctx, r.topic, r.flat)}};
      if (!r.window.is_null()) {
        entry["window"] = r.window;
      }
      arr.push_back(std::move(entry));
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
    // Several series' buckets share ONE response cap (batchBuckets), so the
    // batch is capped tighter than the flat kMaxBatchPaths above: past this
    // many, sharing the cap would coarsen every one of them past usefulness.
    if (paths.size() > kMaxBucketsBatchPaths) {
      return ToolResult::failure(
          "mode 'buckets' takes at most " + std::to_string(kMaxBucketsBatchPaths) +
          " paths per call — they share one response cap. Batch 'stats' first, then ask for buckets on "
          "the series worth the shape.");
    }
    const std::size_t max_points = static_cast<std::size_t>(std::clamp(args.value("max_points", 200), 1, 500));
    // A lone path keeps the shape it has always had, at full single-series
    // resolution — the batch envelope (and its shared, smaller cap) only
    // kicks in once there is more than one series to fit together.
    if (paths.size() == 1) {
      SeriesRead r = readWindowed(paths.front());
      if (!r.ok) {
        return ToolResult::failure(r.error);
      }
      const json stats_json = statsWithDisplayStart(computeStats(r.ts, r.vals), ctx, r.topic, r.flat);
      json base = {{"series", r.path}, {"stats", stats_json}};
      if (!r.window.is_null()) {
        base["window"] = r.window;
      }
      const json out = withCoarsenedBuckets(std::move(base), r.ts, r.vals, max_points);
      return ToolResult::success(out.dump());
    }
    std::vector<SeriesRead> reads;
    reads.reserve(paths.size());
    for (const auto& want : paths) {
      reads.push_back(readWindowed(want));
    }
    const json out = batchBuckets(ctx, reads, max_points);
    return ToolResult::success(out.dump());
  }
  return ToolResult::failure("unknown mode '" + mode + "' (use 'stats' or 'buckets')");
}

// Persistent nodes request exclusion from undo/redo, but only when the SDK
// exposes the bit. A "reserved" substring in the error identifies a host that
// rejects the unknown bit and needs one flags-free retry.
template <typename Fn>
auto createWithFlagFallback(Fn&& fn, bool& degraded) {
  uint32_t flags = 0;
#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT
  flags |= PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;
#endif
  auto result = fn(flags);
  if (!result && flags != 0 && result.error().find("reserved") != std::string::npos) {
    flags = 0;
    result = fn(flags);
    degraded = static_cast<bool>(result);
  }
  return result;
}

#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT
// Confirm that the host persisted the requested property in the node recipe.
bool historyExemptConfirmed(ToolContext& ctx, const std::string& id) {
  auto recipe = ctx.dp.recipeOf(id);
  if (!recipe) {
    return false;
  }
  try {
    return json::parse(*recipe).value("history_exempt", false);
  } catch (const json::exception&) {
    return false;
  }
}
#endif

// createWithFlagFallback, followed by the confirmation probe: a successful
// flagged create is protected only when its recipe confirms it, so
// `undo_protection_unavailable` is set unless that check passes. Skipped when
// the fallback already degraded (that retry already established the answer)
// or when the create itself failed (nothing was persisted to confirm).
template <typename Fn>
auto createHistoryExempt(ToolContext& ctx, const std::string& id, Fn&& fn, bool& undo_protection_unavailable) {
  auto result = createWithFlagFallback(std::forward<Fn>(fn), undo_protection_unavailable);
  if (result && !undo_protection_unavailable) {
#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT
    if (!historyExemptConfirmed(ctx, id)) {
      undo_protection_unavailable = true;
    }
#else
    (void)ctx;
    (void)id;
#endif
  }
  return result;
}

// Adds the disclosure key when the host could not confirm undo/redo
// protection for the node just created.
void annotateUndoProtection(json& result, bool unavailable) {
  if (unavailable) {
    result["undo_protection"] = "unavailable on this host";
  }
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
  // Installing over a name that is already taken is not a judgement call the
  // model gets to make — it silently replaces or shadows something the user has,
  // and neither outcome is what "create" was asked to do. The description used to
  // tell the model to call list_created "before creating something that may
  // already exist"; checking here costs nothing and does not depend on it
  // remembering to.
  if (auto existing = ctx.dp.list()) {
    for (const auto& id : *existing) {
      for (const auto& out : outputs) {
        if (id == out) {
          return ToolResult::failure(
              "'" + out +
              "' already exists — this assistant created it earlier in the session. Remove it first with "
              "remove_derived_series, or choose another name.");
        }
      }
    }
  }

  JoinForecast forecast;
  // How long the output will be when there is no join to shorten it. Taken here,
  // where the input is already resolved, because the catalog does not outlive
  // this block. rowCount() reads the Arrow header — it decodes no values.
  std::optional<std::size_t> single_input_points;
  if (auto catalog = ctx.host.catalogSnapshot()) {
    for (auto& in : inputs) {
      auto lookup = resolveSeriesPath(*catalog, in);
      if (!lookup.resolved) {
        return ToolResult::failure(seriesLookupError(in, lookup));
      }
      if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
        return ToolResult::failure(*blocked);
      }
      in = lookup.resolved->host_path;
      if (inputs.size() == 1) {
        if (auto view = ctx.host.readSeries(lookup.resolved->handle); view) {
          single_input_points = view->rowCount();
        }
      }
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
  bool undo_protection_unavailable = false;
  auto status = createHistoryExempt(
      ctx, name,
      [&](uint32_t flags) {
        return ctx.dp.createTransform(
            name, PJ::Span<const std::string_view>(in_views.data(), in_views.size()),
            PJ::Span<const std::string_view>(out_views.data(), out_views.size()), script, "{}", flags);
      },
      undo_protection_unavailable);
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
  annotateUndoProtection(result, undo_protection_unavailable);
  // Report what the call produced, not what to do about it. This used to carry a
  // "verify_with: read_series on ..." string, and models took the hint: across
  // the creation scenarios it cost 79 extra read_series calls, each one a full
  // round trip re-sending the whole conversation, to learn a number we already
  // had here. Deciding whether a result warrants a second look is the model's
  // job — it knows what the user asked for and this code does not. Ours is to
  // hand it the fact, once, at no cost.
  if (forecast.checked) {
    // Multi-input: the join is what determines the length, and losing most of
    // the rows to it ("of 20000 samples, 340 line up") is the difference between
    // a usable series and a handful of stray points.
    result["points"] = forecast.shared;
    if (forecast.shared < forecast.smallest) {
      result["shortest_input_points"] = forecast.smallest;
    }
  } else if (single_input_points) {
    // Single input: nothing joins, so the output is as long as the input.
    result["points"] = *single_input_points;
  }
  return ToolResult::success(result.dump());
}

// Unique per-call id for evaluate()'s ephemeral node, scoped to this
// plugin's own id namespace on the host (so two plugin instances, or two
// calls in flight, never collide). Never reused — a leftover from a failed
// remove is easy to spot rather than silently shadowed by the next call.
std::atomic<unsigned> g_evaluate_counter{0};

// Run a Luau computation over series and hand back numbers, without leaving
// anything for the user to see: an EPHEMERAL transform is created, read once,
// and removed before returning — the host hides ephemeral outputs from its
// own catalog, but the plugin-facing ABI catalog snapshot still enumerates
// them, which is what makes the read-back possible at all.
ToolResult evaluateSeries(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot evaluate)");
  }
  if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty()) {
    return ToolResult::failure("evaluate requires a non-empty 'inputs' array of topic/field paths");
  }
  const bool has_expr = args.contains("expression") && args["expression"].is_string();
  const bool has_body = args.contains("body") && args["body"].is_string();
  if (!has_expr && !has_body) {
    return ToolResult::failure(
        "evaluate requires 'expression' (a stateless Luau expression over value/v1../time, e.g. "
        "'value - v1') OR 'body' (full Luau statements ending in return, with optional 'global' state)");
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

  // Same resolution, host-create-blocker and join-forecast rules as
  // create_derived_series, and for the same reasons: an unresolved or
  // ambiguous input would install (briefly) a transform that computes
  // nothing meaningful, and inputs that share no timestamps join to zero
  // points either way.
  JoinForecast forecast;
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  for (auto& in : inputs) {
    auto lookup = resolveSeriesPath(*catalog, in);
    if (!lookup.resolved) {
      return ToolResult::failure(seriesLookupError(in, lookup));
    }
    if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
      return ToolResult::failure(*blocked);
    }
    in = lookup.resolved->host_path;
  }
  if (inputs.size() > 1) {
    forecast = forecastJoin(ctx, *catalog, inputs);
    if (forecast.checked && forecast.shared == 0) {
      std::string why = "these inputs share no timestamps, so the computed series would have 0 points: ";
      for (std::size_t i = 0; i < forecast.rates.size(); ++i) {
        why += (i == 0 ? "" : ", ") + forecast.rates[i];
      }
      why +=
          ". evaluate joins multiple inputs on exact timestamp equality, so it only works on series recorded on "
          "the same clock. To relate series that are not (two runs, two devices), read_series each one and "
          "compare the statistics.";
      return ToolResult::failure(why);
    }
  }

  const std::size_t num_extra = inputs.size() - 1;
  const std::string global = args.value("global", std::string{});
  const std::string body =
      has_body ? args["body"].get<std::string>() : "    return (" + args["expression"].get<std::string>() + ")";

  const unsigned call_id = ++g_evaluate_counter;
  const std::string id = "__evaluate_" + std::to_string(call_id);
  // Same "__validate__" id trick create_derived_series uses: the host's
  // validator instantiates the class under that fixed name.
  const std::string validate_script = buildLuauTransform("__validate__", "__validate__", global, body, num_extra);
  if (auto v = ctx.dp.validateScript("transform", ctx.language, validate_script); !v) {
    return ToolResult::failure("invalid expression: " + v.error());
  }
  const std::string script = buildLuauTransform(id, id, global, body, num_extra);

  const std::string output_name = id + "/value";
  std::vector<std::string_view> in_views(inputs.begin(), inputs.end());
  std::array<std::string_view, 1> out_views{output_name};
  auto created = ctx.dp.create(
      id, "transform", ctx.language, PJ::Span<const std::string_view>(in_views.data(), in_views.size()),
      PJ::Span<const std::string_view>(out_views.data(), out_views.size()), script, "{}",
      PJ_DATA_PROCESSOR_FLAG_EPHEMERAL);
  if (!created) {
    return ToolResult::failure("evaluate failed: " + created.error());
  }
  // The node is never shown to the user and must not outlive this call on any
  // path — a successful read, a failed one, or an exception unwinding out of
  // this function. No ctx.notify_data_changed() anywhere here either: nothing
  // changed that the host's GUI should redraw for.
  struct RemoveGuard {
    ToolContext& ctx;
    std::string id;
    ~RemoveGuard() {
      auto status = ctx.dp.remove(id);
      (void)status;  // best-effort teardown of a node the user never saw; nothing to react to here
    }
  } remove_guard{ctx, id};

  const std::string resolved_output = created->empty() ? output_name : created->front();

  // The catalog snapshot taken before create() does not contain the new
  // sink; a fresh one is required to resolve and read it back.
  auto fresh_catalog = ctx.host.catalogSnapshot();
  if (!fresh_catalog) {
    return ToolResult::failure("catalog unavailable after create: " + fresh_catalog.error());
  }
  SeriesRead r = readOne(*fresh_catalog, ctx, resolved_output);
  if (!r.ok) {
    return ToolResult::failure(r.error);
  }
  const SeriesStats stats = computeStats(r.ts, r.vals);
  json result = {{"evaluated", r.path}, {"stats", statsWithDisplayStart(stats, ctx, r.topic, r.flat)}};

  if (args.contains("buckets")) {
    if (!args["buckets"].is_number_integer()) {
      return ToolResult::failure("'buckets' must be an integer between 1 and 500");
    }
    const std::size_t max_points = static_cast<std::size_t>(std::clamp(args["buckets"].get<int>(), 1, 500));
    result = withCoarsenedBuckets(std::move(result), r.ts, r.vals, max_points);
  }
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
      if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
        return ToolResult::failure(*blocked);
      }
      in = lookup.resolved->host_path;
    }
  }
  const std::string output = args.contains("output") && args["output"].is_string()
                                 ? canonicalSeriesPath(args["output"].get<std::string>())
                                 : inputs.front();

  std::vector<std::string_view> input_views(inputs.begin(), inputs.end());
  bool undo_protection_unavailable = false;
  auto topics = createHistoryExempt(
      ctx, "assistant_markers",
      [&](uint32_t flags) {
        return ctx.dp.createMarkers(
            "assistant_markers", PJ::Span<const std::string_view>(input_views.data(), input_views.size()), output, rule,
            "{}", flags);
      },
      undo_protection_unavailable);
  if (!topics) {
    return ToolResult::failure("create_markers failed: " + topics.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  json result = {{"created_markers_on", output}, {"inputs", inputs}, {"form", "rule"}};
  annotateUndoProtection(result, undo_protection_unavailable);
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
  std::string series = canonicalSeriesPath(args["series"].get<std::string>());
  // The rule embeds this name and the host resolves it by itself, so it must
  // be the resolved HOST form — the raw argument used to go straight through,
  // which is how a bare name that exists in two datasets landed on whichever
  // one the host tried first.
  std::string display_series = series;
  if (auto catalog = ctx.host.catalogSnapshot()) {
    auto lookup = resolveSeriesPath(*catalog, series);
    if (!lookup.resolved) {
      return ToolResult::failure(seriesLookupError(series, lookup));
    }
    if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
      return ToolResult::failure(*blocked);
    }
    display_series = lookup.resolved->path;
    series = lookup.resolved->host_path;
  }
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
  bool undo_protection_unavailable = false;
  auto topics = createHistoryExempt(
      ctx, "assistant_markers",
      [&](uint32_t flags) {
        return ctx.dp.createMarkers(
            "assistant_markers", PJ::Span<const std::string_view>(inputs.data(), inputs.size()),
            /*output_marker_topic=*/series, rule, "{}", flags);
      },
      undo_protection_unavailable);
  if (!topics) {
    return ToolResult::failure("create_markers failed: " + topics.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  json result = {{"created_markers_on", display_series}, {"style", style}, {"rule", label}};
  annotateUndoProtection(result, undo_protection_unavailable);
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

// --- playback / viewport executors ------------------------------------------

constexpr const char* kNoPlayback = "the host did not expose pj.playback.v1 (cannot control playback)";
constexpr const char* kNoViewport = "the host did not expose pj.viewport.v1 (cannot zoom plots)";

json stateToJson(const PJ::sdk::PlaybackState& state) {
  return {
      {"playing", state.is_playing},
      {"current_time_s", state.current_time_s},
      {"range", {{"min_s", state.range_min_s}, {"max_s", state.range_max_s}}},
      {"rate", state.playback_rate}};
}

// Full transport snapshot, echoed by every playback mutation so the model sees
// the effect (e.g. a clamped seek) without a follow-up call. Best-effort: a
// failed state read after a SUCCESSFUL mutation self-describes instead of
// failing the tool. All times are display-axis seconds — the numbers on the
// plot X axes.
json playbackStateJson(ToolContext& ctx) {
  auto state = ctx.playback.state();
  if (!state) {
    return {{"state_unavailable", state.error()}};
  }
  return stateToJson(*state);
}

// Shared shell of the playback mutation tools: valid() gate, the host call,
// error wrapping, and the state echo. Arg validation stays per-tool, above.
template <class Op>
ToolResult runPlaybackOp(ToolContext& ctx, const char* verb, Op&& op) {
  if (!ctx.playback.valid()) {
    return ToolResult::failure(kNoPlayback);
  }
  if (auto status = op(); !status) {
    return ToolResult::failure(std::string(verb) + " failed: " + status.error());
  }
  return ToolResult::success(playbackStateJson(ctx).dump());
}

ToolResult playbackTool(const json& args, ToolContext& ctx) {
  const std::string action = args.value("action", std::string());
  if (action.empty()) {
    return ToolResult::failure("playback requires 'action': one of 'state', 'play', 'pause', 'seek', 'rate'");
  }
  if (!ctx.playback.valid()) {
    return ToolResult::failure(kNoPlayback);
  }
  if (action == "state") {
    auto state = ctx.playback.state();
    if (!state) {
      return ToolResult::failure(state.error());
    }
    return ToolResult::success(stateToJson(*state).dump());
  }
  if (action == "play") {
    return runPlaybackOp(ctx, "play", [&] { return ctx.playback.play(); });
  }
  if (action == "pause") {
    return runPlaybackOp(ctx, "pause", [&] { return ctx.playback.pause(); });
  }
  if (action == "seek") {
    if (!args.contains("time_s") || !args["time_s"].is_number()) {
      return ToolResult::failure("seek requires a numeric 'time_s' (display-axis seconds)");
    }
    // The echoed current_time_s shows the host's clamp into the playback range.
    return runPlaybackOp(ctx, "seek", [&] { return ctx.playback.seek(args["time_s"].get<double>()); });
  }
  if (action == "rate") {
    if (!args.contains("rate") || !args["rate"].is_number()) {
      return ToolResult::failure("rate requires a numeric 'rate' (> 0; 1.0 = real time)");
    }
    // Bound the blast radius before the host sees it: a model asking for 0 or
    // 10000x gets the nearest sane speed instead of an error loop.
    const double rate = std::clamp(args["rate"].get<double>(), 0.05, 20.0);
    return runPlaybackOp(ctx, "rate", [&] { return ctx.playback.setPlaybackRate(rate); });
  }
  return ToolResult::failure("unknown playback action '" + action + "'; use state/play/pause/seek/rate");
}

// --- the assistant's own plot tabs -------------------------------------------

constexpr const char* kNoPlotTabs = "the host did not expose pj.plot_tabs.v1 (cannot compose plot tabs)";

// Names of the tabs this assistant currently owns, for the "which are mine?"
// half of an error. An unreadable list degrades to no names rather than
// replacing the real failure with a secondary one.
std::string ownedTabList(ToolContext& ctx) {
  auto ids = ctx.plot_tabs.list();
  if (!ids || ids->empty()) {
    return "none yet";
  }
  std::string out;
  for (const std::string& id : *ids) {
    out += (out.empty() ? "" : ", ") + id;
  }
  return out;
}

// The tab as the HOST holds it, parsed back from tab_config. Every action
// answers with this rather than an echo of the request, so a curve that did not
// land shows as absent instead of being reported as drawn.
json tabReadBack(ToolContext& ctx, const std::string& tab) {
  auto config = ctx.plot_tabs.configOf(tab);
  if (!config) {
    return {{"tab", tab}, {"contents_unavailable", config.error()}};
  }
  json parsed = json::parse(*config, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) {
    return {{"tab", tab}, {"contents_unavailable", "the host returned no readable tab contents"}};
  }
  parsed["tab"] = tab;
  return parsed;
}

ToolResult plotTabTool(const json& args, ToolContext& ctx) {
  const std::string action = args.value("action", std::string());
  if (action.empty()) {
    return ToolResult::failure("plot_tab requires 'action': one of 'create', 'add', 'remove', 'zoom', 'close', 'list'");
  }
  if (!ctx.plot_tabs.valid()) {
    return ToolResult::failure(kNoPlotTabs);
  }
  if (action == "list") {
    auto ids = ctx.plot_tabs.list();
    if (!ids) {
      return ToolResult::failure(ids.error());
    }
    json arr = json::array();
    for (const std::string& id : *ids) {
      arr.push_back(tabReadBack(ctx, id));
    }
    // Owning nothing is an answer, not a failure.
    return ToolResult::success(json({{"count", arr.size()}, {"tabs", arr}}).dump());
  }

  const std::string tab = args.value("tab", std::string());
  if (action == "create") {
    // A name of its own, so the model can address the tab again next turn
    // without having to remember a host-chosen handle.
    const std::string id = tab.empty() ? "view" : tab;
    if (auto status = ctx.plot_tabs.create(id, args.value("title", std::string())); !status) {
      return ToolResult::failure("could not create the tab: " + status.error());
    }
    return ToolResult::success(tabReadBack(ctx, id).dump());
  }
  if (tab.empty()) {
    return ToolResult::failure(
        "'" + action + "' needs 'tab', the name of one of your own tabs (you have: " + ownedTabList(ctx) +
        "). Only tabs you created can be changed; the user's tabs are not yours to touch.");
  }

  if (action == "close") {
    if (auto status = ctx.plot_tabs.close(tab); !status) {
      return ToolResult::failure(status.error() + " (yours: " + ownedTabList(ctx) + ")");
    }
    return ToolResult::success(json({{"closed", tab}}).dump());
  }
  if (action == "zoom") {
    // The two services are meant to be registered together, so this only fires
    // on a host that half-adopted them — worth saying plainly rather than
    // failing as if the tab were at fault.
    if (!ctx.viewport.valid()) {
      return ToolResult::failure(kNoViewport);
    }
    const bool has_start = args.contains("start_s") && args["start_s"].is_number();
    const bool has_end = args.contains("end_s") && args["end_s"].is_number();
    if (!has_start && !has_end) {
      if (auto status = ctx.viewport.zoomReset(); !status) {
        return ToolResult::failure("could not fit the view: " + status.error());
      }
      return ToolResult::success(json({{"tab", tab}, {"x_range", "fit"}}).dump());
    }
    if (!has_start || !has_end) {
      return ToolResult::failure("zoom needs both 'start_s' and 'end_s' (display-axis seconds), or neither to fit");
    }
    const double start_s = args["start_s"].get<double>();
    const double end_s = args["end_s"].get<double>();
    if (start_s >= end_s) {
      return ToolResult::failure("'start_s' must be less than 'end_s'");
    }
    if (auto status = ctx.viewport.zoomToTimeRange(start_s, end_s); !status) {
      return ToolResult::failure("could not zoom: " + status.error());
    }
    return ToolResult::success(json({{"tab", tab}, {"x_range", {{"start_s", start_s}, {"end_s", end_s}}}}).dump());
  }

  if (action != "add" && action != "remove") {
    return ToolResult::failure("unknown plot_tab action '" + action + "'; use create/add/remove/zoom/close/list");
  }
  const std::vector<std::string> paths = requestedPaths(args, {"curves"});
  if (paths.empty()) {
    return ToolResult::failure("'" + action + "' needs 'curves': one topic/field path, or an array of them");
  }
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  const bool adding = action == "add";
  std::vector<std::string> refused;
  std::vector<std::pair<std::string, std::string>> accepted;  // (topic, field) the host took without complaint
  for (const std::string& want : paths) {
    SeriesLookup lookup = resolveSeriesPath(*catalog, want);
    if (!lookup.resolved) {
      refused.push_back(seriesLookupError(want, lookup));
      continue;
    }
    // The host addresses a series by its parts and resolves the dataset itself,
    // so hand it the pieces rather than a joined path it would have to split.
    const std::string topic = lookup.resolved->topic;
    const std::string field = lookup.resolved->host_path.substr(topic.size() + 1);
    auto status = adding ? ctx.plot_tabs.addCurve(tab, topic, field, lookup.resolved->dataset)
                         : ctx.plot_tabs.removeCurve(tab, topic, field, lookup.resolved->dataset);
    if (status) {
      accepted.emplace_back(topic, field);
    } else {
      refused.push_back(status.error());
    }
  }

  json out = tabReadBack(ctx, tab);
  // A call the host accepted is not yet a curve on screen: it may resolve to
  // nothing and simply leave the tab as it was. So the verdict comes from what
  // the tab HOLDS, not from what the calls returned — otherwise this reports a
  // drawing that never happened, which is the one thing the read-back exists to
  // prevent.
  const json& held = out.contains("curves") ? out["curves"] : json::array();
  for (const auto& [topic, field] : accepted) {
    const bool present = std::any_of(held.begin(), held.end(), [&](const json& curve) {
      return curve.value("topic", std::string()) == topic && curve.value("field", std::string()) == field;
    });
    if (present == adding) {
      continue;  // added and there, or removed and gone: what was asked for
    }
    refused.push_back(
        adding ? "'" + topic + "/" + field + "' did not land in the tab"
               : "'" + topic + "/" + field + "' is still drawn");
  }
  if (!refused.empty()) {
    out["refused"] = refused;
  }
  // Nothing landed at all is a failure; a partial landing is a success whose
  // truth the model still has to see.
  return refused.size() == paths.size() ? ToolResult::failure(out.dump()) : ToolResult::success(out.dump());
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

namespace {

// Plain truncation: the full tree, then (if that still does not fit) topic
// names only, with a footer naming the gap. This is the ENTIRE pre-mixed-tier
// catalogDigest, kept verbatim as the fallback for the rare budget so tight
// that not even a bare field COUNT fits for every topic — see catalogDigest's
// count-only feasibility check below, the only caller.
std::string legacyCatalogDigest(const PJ::sdk::CatalogSnapshot& catalog, std::size_t budget_chars) {
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(catalog);

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
  if (catalog.dataSources().size() >= 2) {
    header +=
        "Several datasets are loaded; when the same topic exists in more than one, address the series as "
        "\"<dataset>:<topic>/<field>\".\n";
  }

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

}  // namespace

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
  const std::uint32_t n = static_cast<std::uint32_t>(topics.size());

  // Which dataset each topic belongs to. PJ4 can hold several loaded at once —
  // two runs of the same robot, say — and a flat topic list hides that
  // completely: the model cannot offer to compare them because it does not know
  // there are two, and cannot avoid mixing them for the same reason. Topics are
  // grouped contiguously per source (first_topic/topic_count), so this is a
  // lookup table rather than a scan. Empty when the host reports no sources, in
  // which case the listing stays exactly as it was.
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(*catalog);

  // Dataset-header prefix per topic index — empty except where a new dataset
  // starts. Precomputed once so every tier below (full/partial/count) shares
  // it without re-deriving dataset transitions per topic.
  std::vector<std::string> dataset_prefix(n);
  {
    std::string current_dataset;
    for (std::uint32_t ti = 0; ti < n; ++ti) {
      if (auto it = topic_dataset.find(ti); it != topic_dataset.end() && it->second != current_dataset) {
        current_dataset = it->second;
        dataset_prefix[ti] = "dataset \"" + current_dataset + "\":\n";
      }
    }
  }

  // The dominant field type, when at least 80% of every field in the catalog
  // shares one: that type is then left off every "name (type)" rendering, and
  // only the minority still carries one — the header states the rule once
  // instead of repeating "(float64)" on thirty topics that are all float64.
  std::optional<std::string> dominant_type;
  {
    std::map<std::string, std::size_t> type_counts;
    std::size_t total_fields = 0;
    for (const auto& f : fields) {
      ++type_counts[primitiveTypeName(PJ::sdk::fromAbiType(f.type))];
      ++total_fields;
    }
    if (total_fields > 0) {
      const auto best = std::max_element(
          type_counts.begin(), type_counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
      if (static_cast<double>(best->second) >= 0.8 * static_cast<double>(total_fields)) {
        dominant_type = best->first;
      }
    }
  }
  auto renderField = [&](std::size_t idx) {
    std::string out(PJ::sdk::toStringView(fields[idx].name));
    const char* type_name = primitiveTypeName(PJ::sdk::fromAbiType(fields[idx].type));
    if (!dominant_type || *dominant_type != type_name) {
      out += " (";
      out += type_name;
      out += ")";
    }
    return out;
  };

  // Sub-budget for a partial line's field list (before the "  <topic>: "
  // prefix and the trailing "… +N more") — enough for a handful of short
  // field names at far less than a full line's cost.
  constexpr std::size_t kPartialFieldBudget = 120;

  // Three renderings per topic, cheapest to costliest: count (just how many
  // fields), partial (as many as fit the sub-budget, plus how many more),
  // full (every field). Built once so the assignment pass below only compares
  // sizes, never re-renders.
  std::vector<std::string> full_body(n), partial_body(n), count_body(n);
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    const auto& topic = topics[ti];
    const std::string prefix = "  " + std::string(PJ::sdk::toStringView(topic.name)) + ": ";

    std::string full = prefix;
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      if (fi != 0) {
        full += ", ";
      }
      full += renderField(idx);
    }
    full_body[ti] = full + "\n";

    std::string partial_fields;
    std::uint32_t partial_shown = 0;
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      const std::string candidate = (fi != 0 ? ", " : "") + renderField(idx);
      // Always take at least one field, even an oversized one, so a partial
      // line is never emptier than the count line it is meant to beat.
      if (partial_shown > 0 && partial_fields.size() + candidate.size() > kPartialFieldBudget) {
        break;
      }
      partial_fields += candidate;
      ++partial_shown;
    }
    const std::uint32_t remaining = topic.field_count - partial_shown;
    partial_body[ti] = remaining == 0 ? (prefix + partial_fields + "\n")
                                      : (prefix + partial_fields + ", … +" + std::to_string(remaining) + " more\n");

    count_body[ti] = prefix + std::to_string(topic.field_count) + " fields\n";
  }

  // Feasibility floor: if even the cheapest possible listing — every topic
  // reduced to a bare count — does not fit, there is nothing left to trim
  // except fields entirely, which is exactly what the pre-existing
  // full-tree/names-only truncation already does.
  std::size_t count_total = 0;
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    count_total += dataset_prefix[ti].size() + count_body[ti].size();
  }
  if (count_total > budget_chars) {
    return legacyCatalogDigest(*catalog, budget_chars);
  }

  // Every topic starts at "count" (already paid for above) and ascends
  // cheapest-upgrade-first — (full size - count size), stable-sorted so a tie
  // resolves in catalog order — trying full, then partial, stopping the first
  // time an upgrade would blow the budget. Signed deltas: a topic with no
  // fields renders SHORTER as "full" than as "count", and that must read as
  // an improvement, not wrap around as an unsigned underflow.
  std::vector<int> tier(n, 0);  // 0 = count, 1 = partial, 2 = full
  {
    std::vector<std::uint32_t> order(n);
    for (std::uint32_t ti = 0; ti < n; ++ti) {
      order[ti] = ti;
    }
    auto delta = [](const std::string& upgraded, const std::string& base) {
      return static_cast<long long>(upgraded.size()) - static_cast<long long>(base.size());
    };
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
      return delta(full_body[a], count_body[a]) < delta(full_body[b], count_body[b]);
    });
    long long total = static_cast<long long>(count_total);
    const long long budget = static_cast<long long>(budget_chars);
    for (std::uint32_t ti : order) {
      const long long full_delta = delta(full_body[ti], count_body[ti]);
      if (total + full_delta <= budget) {
        total += full_delta;
        tier[ti] = 2;
        continue;
      }
      const long long partial_delta = delta(partial_body[ti], count_body[ti]);
      if (total + partial_delta <= budget) {
        total += partial_delta;
        tier[ti] = 1;
      }
    }
  }

  std::string body;
  bool any_reduced = false;
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    body += dataset_prefix[ti];
    if (tier[ti] == 2) {
      body += full_body[ti];
    } else if (tier[ti] == 1) {
      body += partial_body[ti];
      any_reduced = true;
    } else {
      body += count_body[ti];
      any_reduced = true;
    }
  }

  std::string header = "Loaded data (" + std::to_string(n) + " topic(s)):\n";
  // Teach the qualifier by stating it where the dataset names are, instead of
  // spending schema tokens on it in every session: this line exists only when
  // several datasets are actually loaded.
  if (catalog->dataSources().size() >= 2) {
    header +=
        "Several datasets are loaded; when the same topic exists in more than one, address the series as "
        "\"<dataset>:<topic>/<field>\".\n";
  }
  if (dominant_type) {
    header += "fields are " + *dominant_type + " unless marked\n";
  }

  std::string footer;
  if (any_reduced) {
    footer +=
        "Some topics list only part of their fields (… +N more) or a field count: describe_topic gives the "
        "rest.\n";
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
       "Read summary statistics ('stats') or a min/max-preserving downsample ('buckets'). Non-finite "
       "values are counted as 'invalid', excluded from min/max/mean/stddev. Bucket times 't' are "
       "seconds relative to the series (or window) start; when the host supports playback control, "
       "stats also carry 't_start_display_s' (where the series starts on the plot axis), so a "
       "bucket's display/seek time = t_start_display_s + t.\n"
       "'paths' is an ARRAY — ask for every series you want stats for in ONE call. A bad path returns "
       "as an error beside the results that worked. buckets takes up to 8 paths (shared cap, ~100 "
       "buckets each for 4-6 series); t_start_s/t_end_s (display axis) narrow the read and re-base t.",
       {{"type", "object"},
        {"properties",
         {{"paths",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "topic/field paths; a bare string is accepted for a single series"}}},
          {"mode", {{"type", "string"}, {"enum", json::array({"stats", "buckets"})}}},
          {"max_points", {{"type", "integer"}, {"description", "bucket count for mode=buckets (<=500)"}}},
          {"t_start_s", {{"type", "number"}, {"description", "display-axis window start (optional)"}}},
          {"t_end_s", {{"type", "number"}, {"description", "display-axis window end (optional)"}}}}},
        {"required", json::array({"paths"})}},
       &readSeriesTool});

  add(
      {"evaluate",
       "Run a Luau computation over series and get numbers back — nothing is created or shown. Same "
       "inputs/expression/body/global as create_derived_series. Returns stats (min/max with their "
       "times, invalid count); add 'buckets' for the shape too. Use to answer how much/when/whether "
       "before deciding if anything is worth creating.",
       {{"type", "object"},
        {"properties",
         {{"inputs", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"expression", {{"type", "string"}}},
          {"body", {{"type", "string"}}},
          {"global", {{"type", "string"}}},
          {"buckets", {{"type", "integer"}, {"description", "1-500"}}}}},
        {"required", json::array({"inputs"})}},
       &evaluateSeries});

  add(
      {"create_derived_series",
       "Create a new derived timeseries computed live from one or more inputs. Each sample sees "
       "'value' (first input), 'v1'..'vN' (further inputs, in order), and 'time' (seconds). Use "
       "'expression' for a stateless formula (e.g. 'value * 2'); for stateful transforms like a "
       "derivative, use 'global' (runs once, persistent locals) + 'body' (statements ending in "
       "return; return nothing to suppress a sample). Saved in layouts. Undo protection is "
       "verified when supported; failure is reported.",
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
       "remove_markers to clear it). Saved in layouts. Undo protection is verified when supported; "
       "failure is reported.\n"
       "A condition true over a STRETCH of time wants ONE region per stretch "
       "(startMarker/closeMarker), never one line per matching sample. The call reports how many "
       "markers it produced and of which kind: past ~50, the shape is wrong — merge contiguous "
       "hits into regions or raise the threshold.\n"
       "A per-sample threshold on a raw high-rate signal (IMU and the like) marks vibration, not "
       "events: require the condition to hold for a minimum duration and cross-check against an "
       "independent slower signal before labeling.\n"
       "RAW FORM: 'inputs' (series the rule reads) + 'rule' (a whole Luau script, run once over "
       "the full series) + optional 'output' (series path the markers attach to; default = first "
       "input; \"__global__\" renders on every plot — only for events meaningful against ANY "
       "signal).\n"
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
       "to find the name to remove. A name that is already taken is refused by create_derived_series "
       "itself, so there is no need to call this first.",
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
      {"playback",
       "Drive the transport: the time cursor, shared by every plot. action: 'state' (read it), "
       "'play', 'pause', 'seek' (to 'time_s'), 'rate' (to 'rate'; 1.0 = real time, clamped to "
       "[0.05, 20]). Every call returns the whole state - playing, current_time_s, range "
       "{min_s, max_s}, rate - so a clamped seek shows where the cursor actually landed. Times are "
       "DISPLAY-AXIS SECONDS, the numbers on the plot X axes: a feature found in read_series "
       "buckets sits at stats.t_start_display_s + bucket.t.",
       {{"type", "object"},
        {"properties",
         {{"action", {{"type", "string"}, {"enum", json::array({"state", "play", "pause", "seek", "rate"})}}},
          {"time_s", {{"type", "number"}, {"description", "target time, display-axis seconds (seek)"}}},
          {"rate", {{"type", "number"}, {"description", "speed multiplier, > 0 (rate)"}}}}},
        {"required", json::array({"action"})}},
       &playbackTool});

  add(
      {"plot_tab",
       "Compose plot tabs of your own. A tab you create is watermarked \"AI\" and is the only place "
       "you may draw: the user's tabs are not yours to fill, zoom or close, and they do not go away "
       "when you close yours. Supporting hosts save your tabs with the layout and exclude them from "
       "undo/redo; older hosts may keep them only for the session.\n"
       "action: 'create' (optional 'tab' name and 'title') | 'add'/'remove' ('curves', topic/field "
       "paths) | 'zoom' ('start_s'..'end_s' display-axis seconds; omit both to fit) | 'close' | "
       "'list'. Every action answers with the tab as the host holds it, so a curve that did not "
       "land shows as missing instead of being reported as drawn. A series you made with "
       "create_derived_series is addressable here as 'name/value'.",
       {{"type", "object"},
        {"properties",
         {{"action", {{"type", "string"}, {"enum", json::array({"create", "add", "remove", "zoom", "close", "list"})}}},
          {"tab", {{"type", "string"}, {"description", "your name for the tab"}}},
          {"title", {{"type", "string"}, {"description", "tab title shown to the user (create)"}}},
          {"curves", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "topic/field paths"}}},
          {"start_s", {{"type", "number"}}},
          {"end_s", {{"type", "number"}}}}},
        {"required", json::array({"action"})}},
       &plotTabTool});

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

nlohmann::json ToolRegistry::toFunctionSpecs() const {
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
