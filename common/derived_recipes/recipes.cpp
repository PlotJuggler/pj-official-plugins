// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "derived_recipes/recipes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <pj_base/builtin/builtin_object.hpp>

namespace derived_recipes {

std::string luaStringEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string buildOnDemandChunk(const std::string& body) {
  std::string src = "-- pj-script: luau\n";
  src += "local inputs, params = ...\n";
  src += body + "\n";
  return src;
}

std::string buildLuauTransform(
    const std::string& id, const std::string& name, const std::string& global_code, const std::string& body,
    std::size_t num_extra) {
  std::string params = "time, value";
  for (std::size_t k = 0; k < num_extra; ++k) {
    params += ", v" + std::to_string(k + 1);
  }

  std::string src = "-- pj-script: luau\n";
  src += "local function _pj_make()\n";
  src += global_code + "\n";
  src += "  return function(" + params + ")\n";
  src += body + "\n";
  src += "  end\n";
  src += "end\n";
  src += "local T = { id = \"" + luaStringEscape(id) + "\", name = \"" + luaStringEscape(name) +
         "\", output = \"double\" }\n";
  src += "T.__index = T\n";
  src += "function T.create(_) return setmetatable({ fn = _pj_make() }, T) end\n";
  src += "function T:calculate(t, v, ...) return self.fn(t, v, ...) end\n";
  src += "return T\n";
  return src;
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
std::map<std::uint32_t, std::string> datasetByTopicIndex(std::span<const PJ_data_source_info_t> sources) {
  std::map<std::uint32_t, std::string> out;
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

std::map<std::uint32_t, std::string> datasetByTopicIndex(const PJ::sdk::CatalogSnapshot& catalog) {
  return datasetByTopicIndex(catalog.dataSources());
}

// True for a marker set's own object topic ("__markers__/<series>" or
// "__markers__/__global__") -- excluded everywhere a model would otherwise
// see it as a readable object.
bool isMarkerObjectTopic(std::string_view name) {
  return name.substr(0, PJ::sdk::kMarkerObjectTopicPrefix.size()) == PJ::sdk::kMarkerObjectTopicPrefix;
}

// The dataset an object topic's `source` handle belongs to, or empty when it
// matches none (a host reporting no sources, same degraded case
// datasetByTopicIndex documents for scalar topics).
std::string objectTopicDatasetName(std::span<const PJ_data_source_info_t> sources, PJ_data_source_handle_t source) {
  for (const auto& src : sources) {
    if (src.handle.id == source.id) {
      return std::string(PJ::sdk::toStringView(src.name));
    }
  }
  return {};
}

// "dataset:name" when `dataset` is non-empty, else `name` unchanged — the
// host's qualifier convention, applied everywhere a catalog-derived name is
// handed back to the model.
std::string qualifyWithDataset(const std::string& dataset, const std::string& name) {
  return dataset.empty() ? name : dataset + ":" + name;
}

QualifierMatch matchDatasetQualifier(const PJ::sdk::CatalogSnapshot& catalog, std::string_view series) {
  const auto topics = catalog.topics();
  const auto sources = catalog.dataSources();
  QualifierMatch out{std::string(series), 0, static_cast<std::uint32_t>(topics.size())};
  std::size_t qualifier_len = 0;
  for (const auto& src : sources) {
    const std::string name(PJ::sdk::toStringView(src.name));
    if (name.empty() || name.size() <= qualifier_len || series.size() <= name.size() || series[name.size()] != ':' ||
        series.compare(0, name.size(), name) != 0) {
      continue;
    }
    qualifier_len = name.size();
    out.topic_lo = src.first_topic;
    out.topic_hi = std::min(src.first_topic + src.topic_count, static_cast<std::uint32_t>(topics.size()));
  }
  if (qualifier_len != 0) {
    out.bare = std::string(series.substr(qualifier_len + 1));
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

namespace {

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
SeriesLookup resolveSeriesPath(
    const PJ::sdk::CatalogSnapshot& catalog, const std::string& series, const TopicDatasetMap& topic_dataset) {
  SeriesLookup out;
  auto topics = catalog.topics();
  auto fields = catalog.fields();

  const QualifierMatch qm = matchDatasetQualifier(catalog, series);
  const std::string& bare = qm.bare;

  const auto want = pathSegments(bare);
  auto dataset_of = [&](std::uint32_t ti) {
    const auto it = topic_dataset.find(ti);
    return it == topic_dataset.end() ? std::string() : it->second;
  };
  auto qualified = [&](std::uint32_t ti, const std::string& full) { return qualifyWithDataset(dataset_of(ti), full); };

  std::optional<ResolvedSeries> exact;
  bool exact_ambiguous = false;
  std::vector<std::string> exact_candidates;
  std::optional<ResolvedSeries> fuzzy;
  for (std::uint32_t ti = qm.topic_lo; ti < qm.topic_hi; ++ti) {
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

SeriesLookup resolveSeriesPath(const PJ::sdk::CatalogSnapshot& catalog, const std::string& series) {
  return resolveSeriesPath(catalog, series, datasetByTopicIndex(catalog));
}

// The error a failed lookup should produce: self-contained, so the model can
// fix the path from the message alone.
std::string seriesLookupError(const std::string& series, const SeriesLookup& lookup, Audience audience) {
  if (lookup.ambiguous) {
    std::string msg = "'" + series + "' is ambiguous; it matches ";
    for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
      msg += (i != 0 ? ", " : "");
      msg += "'" + lookup.candidates[i] + "'";
    }
    msg += ". Use the full path.";
    return msg;
  }
  if (audience == Audience::kUser) {
    return "unknown series '" + series + "'; pick a series or object topic from the list.";
  }
  return "unknown series '" + series +
         "'. If you expected it to exist, call list_topics (with a filter) or describe_topic once to check before "
         "concluding it is missing.";
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

// Resolve one input against the object half of the v2 catalog: an optional
// "dataset:" qualifier (matched against known source names, longest match
// wins, same convention matchDatasetQualifier uses for scalar paths) narrows
// the search; the bare remainder is matched by exact name against
// objectTopics(), excluding marker topics (drawn, not read). Ambiguous only
// when an UNQUALIFIED bare name matches object topics on several datasets.
std::vector<ObjectTopicEntry> listObjectTopics(const PJ::sdk::CatalogSnapshotV2& v2) {
  std::vector<ObjectTopicEntry> out;
  const auto sources = v2.dataSources();
  const bool multi_dataset = sources.size() >= 2;
  for (const auto& obj : v2.objectTopics()) {
    ObjectTopicEntry e;
    e.name = std::string(PJ::sdk::toStringView(obj.name));
    if (isMarkerObjectTopic(e.name)) {
      continue;
    }
    e.type = std::string(PJ::sdk::toStringView(obj.builtin_object_type));
    e.dataset = objectTopicDatasetName(sources, obj.source);
    e.qualified = qualifyWithDataset(multi_dataset ? e.dataset : std::string(), e.name);
    e.info = obj;
    out.push_back(std::move(e));
  }
  return out;
}

ObjectLookup resolveObjectTopic(const PJ::sdk::CatalogSnapshotV2& v2, const std::string& want) {
  ObjectLookup out;
  const auto sources = v2.dataSources();
  std::string bare = want;
  std::size_t qualifier_len = 0;
  PJ_data_source_handle_t qual_source{};
  bool has_qualifier = false;
  for (const auto& src : sources) {
    const std::string name(PJ::sdk::toStringView(src.name));
    if (name.empty() || name.size() <= qualifier_len || want.size() <= name.size() || want[name.size()] != ':' ||
        want.compare(0, name.size(), name) != 0) {
      continue;
    }
    qualifier_len = name.size();
    qual_source = src.handle;
    has_qualifier = true;
  }
  if (has_qualifier) {
    bare = want.substr(qualifier_len + 1);
  }

  std::optional<ResolvedEvalInput> found;
  bool ambiguous = false;
  for (const auto& obj : v2.objectTopics()) {
    const std::string name(PJ::sdk::toStringView(obj.name));
    if (isMarkerObjectTopic(name) || name != bare) {
      continue;
    }
    if (has_qualifier && obj.source.id != qual_source.id) {
      continue;
    }
    ResolvedEvalInput r;
    r.host_path = name;
    r.display_path = qualifyWithDataset(objectTopicDatasetName(sources, obj.source), name);
    r.is_object = true;
    r.object_type = std::string(PJ::sdk::toStringView(obj.builtin_object_type));
    r.source = obj.source;
    r.has_source = true;
    r.entry_count = obj.entry_count;
    r.time_min_ns = obj.time_min_ns;
    r.time_max_ns = obj.time_max_ns;
    r.info = obj;
    if (out.candidates.size() < kMaxCandidates) {
      out.candidates.push_back(r.display_path);
    }
    if (!found) {
      found = std::move(r);
    } else {
      ambiguous = true;
    }
  }
  if (ambiguous) {
    out.ambiguous = true;
    return out;
  }
  if (found) {
    out.resolved = std::move(found);
    out.candidates.clear();
  }
  return out;
}

std::string objectLookupError(const std::string& want, const ObjectLookup& lookup) {
  std::string msg = "'" + want + "' is ambiguous; it matches ";
  for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
    msg += (i != 0 ? ", " : "") + ("'" + lookup.candidates[i] + "'");
  }
  msg += ". Use the full path.";
  return msg;
}

ResolvedEvalInputs resolveEvalInputs(
    PJ::sdk::ToolboxHostView& host, const PJ::sdk::CatalogSnapshotV2& v2, const std::vector<std::string>& raw_inputs,
    Audience audience) {
  ResolvedEvalInputs out;
  for (const auto& in : raw_inputs) {
    ObjectLookup obj_lookup = resolveObjectTopic(v2, in);
    if (obj_lookup.ambiguous) {
      out.error = objectLookupError(in, obj_lookup);
      return out;
    }
    if (obj_lookup.resolved) {
      if (!out.anchor_source) {
        out.anchor_source = obj_lookup.resolved->source;
      }
      obj_lookup.resolved->aliases = {in, obj_lookup.resolved->host_path};
      out.inputs.push_back(std::move(*obj_lookup.resolved));
      continue;
    }
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      out.error = "catalog unavailable: " + catalog.error();
      return out;
    }
    auto lookup = resolveSeriesPath(*catalog, in);
    if (!lookup.resolved) {
      out.error = seriesLookupError(in, lookup, audience);
      return out;
    }
    ResolvedEvalInput r;
    r.host_path = lookup.resolved->host_path;
    r.display_path = lookup.resolved->path;
    r.aliases = {in, r.host_path};
    r.is_object = false;
    if (auto handle = dataSourceHandleFor(*catalog, lookup.resolved->dataset)) {
      r.source = *handle;
      r.has_source = true;
      if (!out.anchor_source) {
        out.anchor_source = *handle;
      }
    }
    out.inputs.push_back(std::move(r));
  }
  for (auto& input : out.inputs) {
    if (input.has_source && out.anchor_source && input.source.id != out.anchor_source->id) {
      out.error = "on-demand inputs must belong to one dataset; cannot combine different datasets";
      return out;
    }
    const std::string dataset = objectTopicDatasetName(v2.dataSources(), input.source);
    input.request_path = qualifyWithDataset(dataset, input.host_path);
    input.aliases.push_back(input.request_path);
    for (const auto& alias : input.aliases) {
      const auto [existing, inserted] = out.aliases.emplace(alias, input.request_path);
      if (!inserted && existing->second != input.request_path) {
        out.error = "ambiguous input alias '" + alias + "'; use distinct input paths";
        return out;
      }
    }
  }
  return out;
}

std::string indentPython(const std::string& code) {
  if (code.find_first_not_of(" \t\r\n") == std::string::npos) {
    return "    pass\n";
  }
  std::string out;
  std::size_t start = 0;
  while (start <= code.size()) {
    const std::size_t nl = code.find('\n', start);
    const std::string line = code.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    out += line.empty() ? "\n" : "    " + line + "\n";
    if (nl == std::string::npos) {
      break;
    }
    start = nl + 1;
  }
  return out;
}

namespace {

// The table that rebuilds `inputs` under every alias a script may use, so a dataset-qualified key and
// the historical bare one both work without textual substitution in user code. Building a fresh table
// also avoids overwriting another input key. Luau always gets the table; Python only when there are
// aliases.
std::string buildAliasTable(const ResolvedEvalInputs& resolved, bool python) {
  if (python && resolved.aliases.empty()) {
    return {};
  }
  std::string table = "inputs = {\n";
  for (const auto& [alias, key] : resolved.aliases) {
    const std::string value = "inputs[\"" + luaStringEscape(key) + "\"]";
    table += python ? "\"" + luaStringEscape(alias) + "\": " + value + ",\n"
                    : "[\"" + luaStringEscape(alias) + "\"] = " + value + ",\n";
  }
  table += "}\n";
  return table;
}

}  // namespace

std::string buildResolvedOnDemandChunk(const std::string& body, const ResolvedEvalInputs& resolved) {
  return buildOnDemandChunk(buildAliasTable(resolved, /*python=*/false) + body);
}

std::string buildOnDemandChunkPython(const std::string& body, const ResolvedEvalInputs& resolved) {
  std::string src = "# pj-script: python\n";
  src += "def evaluate(inputs, params):\n";
  src += indentPython(buildAliasTable(resolved, /*python=*/true) + body);
  return src;
}

std::string buildVariablePrologue(std::string_view language, const std::vector<InputBinding>& bindings) {
  const bool python = language == "python";
  std::string out;
  for (const auto& binding : bindings) {
    out += std::string(python ? "" : "local ") + binding.var + " = inputs[\"" + luaStringEscape(binding.key) + "\"]\n";
  }
  return out;
}

std::string inferredVariableName(std::string_view topic, const std::set<std::string>& taken) {
  std::string leaf(topic);
  while (!leaf.empty() && leaf.back() == '/') {
    leaf.pop_back();
  }
  if (const std::size_t slash = leaf.rfind('/'); slash != std::string::npos) {
    leaf = leaf.substr(slash + 1);
  } else if (const std::size_t colon = leaf.rfind(':'); colon != std::string::npos) {
    leaf = leaf.substr(colon + 1);
  }
  std::string name;
  for (const char ch : leaf) {
    const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
    const char mapped = ok ? ch : '_';
    if (mapped == '_' && !name.empty() && name.back() == '_') {
      continue;  // one underscore for a run of separators
    }
    name.push_back(mapped);
  }
  while (!name.empty() && name.back() == '_') {
    name.pop_back();
  }
  if (name.empty()) {
    name = "input";
  } else if (std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
    name = "input_" + name;
  }
  if (isReservedScriptName(name)) {
    name += "_";
  }
  std::string candidate = name;
  for (int n = 2; taken.count(candidate) != 0; ++n) {
    candidate = name + "_" + std::to_string(n);
  }
  return candidate;
}

namespace {

bool isWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The index after the string whose opening quote is at `at`.
std::size_t skipQuoted(std::string_view text, std::size_t at, bool python) {
  const char quote = text[at];
  const std::size_t n = text.size();
  if (python && text.compare(at, 3, std::string(3, quote)) == 0) {
    const std::size_t close = text.find(std::string(3, quote), at + 3);
    return close == std::string_view::npos ? n : close + 3;
  }
  std::size_t k = at + 1;
  while (k < n && text[k] != quote && text[k] != '\n') {
    k += text[k] == '\\' ? 2 : 1;
  }
  return std::min(k + 1, n);
}

// `at` is on a '[': the index after a [[...]] / [=[...]=] block, or `at` when there is none.
std::size_t skipLuaLong(std::string_view text, std::size_t at) {
  const std::size_t n = text.size();
  std::size_t k = at + 1;
  std::size_t level = 0;
  while (k < n && text[k] == '=') {
    ++level;
    ++k;
  }
  if (k >= n || text[k] != '[') {
    return at;
  }
  const std::size_t close = text.find("]" + std::string(level, '=') + "]", k + 1);
  return close == std::string_view::npos ? n : close + level + 2;
}

std::vector<CodeToken> tokenize(std::string_view text, std::string_view language) {
  const bool python = language == "python";
  const std::size_t n = text.size();
  std::vector<CodeToken> out;
  std::size_t i = 0;
  while (i < n) {
    const char c = text[i];
    CodeToken token{CodeTokenKind::kOther, i, i + 1};
    if (c == '"' || c == '\'') {
      token = {CodeTokenKind::kString, i, skipQuoted(text, i, python)};
    } else if (python && c == '#') {
      std::size_t end = i;
      while (end < n && text[end] != '\n') {
        ++end;
      }
      token = {CodeTokenKind::kComment, i, end};
    } else if (!python && c == '-' && i + 1 < n && text[i + 1] == '-') {
      const std::size_t after = i + 2 < n && text[i + 2] == '[' ? skipLuaLong(text, i + 2) : i + 2;
      std::size_t end = after;
      if (after == i + 2) {
        while (end < n && text[end] != '\n') {
          ++end;
        }
      }
      token = {CodeTokenKind::kComment, i, end};
    } else if (!python && c == '[' && skipLuaLong(text, i) != i) {
      token = {CodeTokenKind::kString, i, skipLuaLong(text, i)};
    } else if (isWordChar(c)) {
      std::size_t end = i;
      while (end < n && isWordChar(text[end])) {
        ++end;
      }
      token = {CodeTokenKind::kWord, i, end};
    }
    out.push_back(token);
    i = token.end;
  }
  return out;
}

}  // namespace

void forEachCodeToken(
    std::string_view text, std::string_view language, const std::function<void(const CodeToken&)>& fn) {
  for (const CodeToken& token : tokenize(text, language)) {
    fn(token);
  }
}

std::size_t returnArity(const std::string& body, const std::string& language) {
  const bool python = language == "python";
  const std::vector<CodeToken> tokens = tokenize(body, language);
  const std::string_view text = body;
  std::size_t best = 0;
  std::size_t t = 0;
  while (t < tokens.size()) {
    const CodeToken& head = tokens[t];
    if (head.kind != CodeTokenKind::kWord || text.substr(head.begin, head.end - head.begin) != "return") {
      ++t;
      continue;
    }
    // The expression list after `return`.
    int depth = 0;
    std::size_t commas = 0;
    bool any = false;
    char last = '\0';  // the last significant byte: a Lua list may continue on the next line after a ','
    std::size_t k = t + 1;
    for (; k < tokens.size(); ++k) {
      const CodeToken& tok = tokens[k];
      if (tok.kind == CodeTokenKind::kComment) {
        if (python && depth == 0) {
          break;
        }
        continue;
      }
      if (tok.kind == CodeTokenKind::kString) {
        any = true;
        last = '"';
        continue;
      }
      if (tok.kind == CodeTokenKind::kWord) {
        const std::string_view word = text.substr(tok.begin, tok.end - tok.begin);
        if (depth == 0 && !python && (word == "end" || word == "else" || word == "elseif" || word == "until")) {
          break;
        }
        any = true;
        last = 'w';
        continue;
      }
      const char d = text[tok.begin];
      if (d == '(' || d == '[' || d == '{') {
        any = true;
        ++depth;
        last = d;
      } else if (d == ')' || d == ']' || d == '}') {
        --depth;
        last = d;
      } else if (depth == 0 && d == ';') {
        break;
      } else if (depth == 0 && d == '\n') {
        if (python || last != ',') {
          break;
        }
      } else if (depth == 0 && d == ',') {
        ++commas;
        last = d;
      } else if (std::isspace(static_cast<unsigned char>(d)) == 0) {
        any = true;
        last = d;
      }
    }
    if (any) {
      best = std::max(best, commas + 1);
    }
    t = std::max(k, t + 1);
  }
  return std::clamp<std::size_t>(best, 1, 8);
}

namespace {

// Calls fn(word_begin, word_end) for every identifier use of `text`: words that are not strings or
// comments, not a field after `.` (or a method after `:` in Luau), not a Luau table key and not a
// Python keyword argument.
void forEachIdentifierUse(
    std::string_view text, std::string_view language, const std::function<void(std::size_t, std::size_t)>& fn) {
  const bool python = language == "python";
  const std::vector<CodeToken> tokens = tokenize(text, language);
  // Neighbouring significant (non-blank, non-comment) tokens of tokens[i].
  const auto significant = [&](std::size_t from, int step) -> const CodeToken* {
    for (std::ptrdiff_t k = static_cast<std::ptrdiff_t>(from) + step;
         k >= 0 && k < static_cast<std::ptrdiff_t>(tokens.size()); k += step) {
      const CodeToken& tok = tokens[static_cast<std::size_t>(k)];
      if (tok.kind == CodeTokenKind::kComment ||
          (tok.kind == CodeTokenKind::kOther && std::isspace(static_cast<unsigned char>(text[tok.begin])) != 0)) {
        continue;
      }
      return &tok;
    }
    return nullptr;
  };
  const auto is_punct = [&](const CodeToken* tok, char c) {
    return tok != nullptr && tok->kind == CodeTokenKind::kOther && text[tok->begin] == c;
  };
  std::vector<char> brackets;  // the open brackets, innermost last
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const CodeToken& tok = tokens[i];
    if (tok.kind == CodeTokenKind::kOther) {
      const char c = text[tok.begin];
      if (c == '(' || c == '[' || c == '{') {
        brackets.push_back(c);
      } else if ((c == ')' || c == ']' || c == '}') && !brackets.empty()) {
        brackets.pop_back();
      }
      continue;
    }
    if (tok.kind != CodeTokenKind::kWord) {
      continue;
    }
    const CodeToken* prev = significant(i, -1);
    const CodeToken* next = significant(i, +1);
    if (is_punct(prev, '.') || (!python && is_punct(prev, ':'))) {
      continue;  // a field or a method, not a variable ("..", the concatenation, is rare before a name)
    }
    // `name =` (not `==`) as a key of a table constructor (Luau) or a keyword argument (Python).
    if (is_punct(next, '=')) {
      const CodeToken* after = significant(static_cast<std::size_t>(next - tokens.data()), +1);
      const bool single = !is_punct(after, '=') || after->begin != next->end;
      const char inner = brackets.empty() ? '\0' : brackets.back();
      const bool key_position =
          python ? inner == '(' : (inner == '{' && (is_punct(prev, '{') || is_punct(prev, ',') || is_punct(prev, ';')));
      if (single && key_position) {
        continue;
      }
    }
    fn(tok.begin, tok.end);
  }
}

}  // namespace

bool mentionsIdentifier(std::string_view text, std::string_view language, std::string_view word) {
  bool found = false;
  forEachIdentifierUse(text, language, [&](std::size_t begin, std::size_t end) {
    found = found || text.substr(begin, end - begin) == word;
  });
  return found;
}

std::string renameIdentifiers(
    std::string_view text, std::string_view language, const std::map<std::string, std::string>& renames) {
  std::string out;
  out.reserve(text.size());
  std::size_t copied = 0;
  forEachIdentifierUse(text, language, [&](std::size_t begin, std::size_t end) {
    const auto it = renames.find(std::string(text.substr(begin, end - begin)));
    if (it == renames.end()) {
      return;
    }
    out.append(text.substr(copied, begin - copied));
    out += it->second;
    copied = end;
  });
  out.append(text.substr(copied));
  return out;
}

bool isReservedScriptName(std::string_view name, std::string_view language) {
  static const std::set<std::string, std::less<>> kLuau = {
      "and", "break", "continue", "do",  "else", "elseif", "end",    "export", "false", "for",   "function", "if",
      "in",  "local", "nil",      "not", "or",   "repeat", "return", "then",   "true",  "until", "while",    "type"};
  static const std::set<std::string, std::less<>> kPython = {
      "False",    "None",   "True",  "and",  "as",     "assert",   "async",   "await", "break", "class",
      "continue", "def",    "del",   "elif", "else",   "except",   "finally", "for",   "from",  "global",
      "if",       "import", "in",    "is",   "lambda", "nonlocal", "not",     "or",    "pass",  "raise",
      "return",   "try",    "while", "with", "yield",  "match",    "case"};
  // names the generated chunk and the standard libraries use
  static const std::set<std::string, std::less<>> kShared = {
      "inputs",   "params", "pj",    "math",  "string", "table", "print",  "pairs",     "ipairs", "tostring",
      "tonumber", "select", "error", "os",    "bit32",  "utf8",  "buffer", "coroutine", "vector", "len",
      "range",    "str",    "int",   "float", "list",   "dict",  "min",    "max",       "abs"};
  if (kShared.count(name) != 0) {
    return true;
  }
  const bool luau = language != "python";
  const bool python = language != "luau";
  return (luau && kLuau.count(name) != 0) || (python && kPython.count(name) != 0);
}

std::string varNameError(std::string_view name, std::string_view language, const std::vector<std::string>& other_vars) {
  const std::string shown(name);
  const bool identifier = !name.empty() && std::isdigit(static_cast<unsigned char>(name.front())) == 0 &&
                          std::all_of(name.begin(), name.end(), [](char c) { return isWordChar(c); });
  if (!identifier) {
    return "'" + shown + "' is not a valid name: use letters, digits and _, and do not start with a digit.";
  }
  if (isReservedScriptName(name, language)) {
    return "'" + shown + "' is reserved by the language or by the script; pick another name.";
  }
  if (std::find(other_vars.begin(), other_vars.end(), shown) != other_vars.end()) {
    return "'" + shown + "' is already the name of another input.";
  }
  return {};
}

namespace {

// How many lines `text` takes when a line break follows it ("a\nb" -> 2, "" -> 1).
int physicalLines(const std::string& text) {
  return 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

// The line-break-terminated lines of `prefix`.
int terminatedLines(const std::string& prefix) {
  return static_cast<int>(std::count(prefix.begin(), prefix.end(), '\n'));
}

// Escape a string for a DOUBLE-QUOTED Luau or Python literal. A user-controlled id or name with a quote,
// a backslash or a line break would otherwise close the literal early and the rest would be parsed as
// CODE (a name like `a"; import os; ...` would run when the generated script is compiled). Both
// languages accept the same C-style escapes for these characters.
std::string escapeForStringLiteral(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

}  // namespace

BuiltScript buildOnDemandScript(
    const std::string& body, const std::string& globals, const std::vector<InputBinding>& bindings,
    const ResolvedEvalInputs& resolved, std::string_view language) {
  const bool python = language == "python";
  const std::string prefix = buildAliasTable(resolved, python) + buildVariablePrologue(language, bindings);
  const std::string user = globals.empty() ? body : globals + "\n" + body;
  BuiltScript out;
  int line = 1;  // the line the next part starts on
  if (python) {
    out.script = "# pj-script: python\ndef evaluate(inputs, params):\n" + indentPython(prefix + user);
    line += 2;
  } else {
    out.script = buildOnDemandChunk(prefix + user);
    line += 2;
  }
  line += terminatedLines(prefix);
  if (!globals.empty()) {
    out.layout.globals_first_line = line;
    out.layout.globals_lines = physicalLines(globals);
    line += out.layout.globals_lines;
  }
  out.layout.body_first_line = line;
  out.layout.body_lines = physicalLines(body);
  return out;
}

// Build a complete self-describing Luau FILTER CLASS the host can compile and run
// live as a DerivedEngine node (via createTransform). The user's global code runs
// once per instance inside a factory closure, so its locals persist across calls
// (PJ3 global-variable semantics); the body becomes the per-sample function with
// `time`/`value` (and `v1..vN` for the additional sources) in scope.
//
// `num_extra` is the count of additional sources: the body function takes
// `(time, value, v1, …, v<num_extra>)`, matching the host's MIMO calculate contract
// (calculate(self, t, v, v1..vN-1) — see pj_scripting FILTER_CLASS.md). `:calculate`
// forwards its args with `...`, so any arity works; the named params just give the
// body the v1..vN identifiers. Output count is decided host-side by the `outputs`
// passed to createTransform — `:calculate` returns the body's results unchanged
// (MULTRET), so a body that `return`s M values feeds M output topics.
BuiltScript buildTransformScript(
    const std::string& raw_id, const std::string& raw_name, const std::string& global_code, const std::string& body,
    std::size_t num_extra, std::string_view language) {
  const std::string id = escapeForStringLiteral(raw_id);
  const std::string name = escapeForStringLiteral(raw_name);
  std::string params = "time, value";
  for (std::size_t k = 0; k < num_extra; ++k) {
    params += ", v" + std::to_string(k + 1);
  }
  BuiltScript out;
  ScriptLayout& layout = out.layout;
  if (language == "python") {
    // A module with a top-level class `T`. The global section runs once at module level (so `global`
    // persistent state works, PJ3-style); the body becomes the function, indented one level.
    std::string src = "# pj-script: python\n";
    int line = 2;
    if (!global_code.empty()) {
      src += global_code + "\n\n";
      layout.globals_first_line = line;
      layout.globals_lines = physicalLines(global_code);
      line += layout.globals_lines + 1;
    }
    src += "def _pj_fn(" + params + "):\n";
    ++line;
    const std::string user = body.empty() ? "return value" : body;
    src += indentPython(user);
    layout.body_first_line = line;
    layout.body_lines = physicalLines(user);
    src += "\n";
    src += "class T:\n";
    src += "    id = \"" + id + "\"\n";
    src += "    name = \"" + name + "\"\n";
    src += "    output = \"double\"\n";
    src += "    @staticmethod\n";
    src += "    def create(params):\n        return T()\n";
    src += "    def calculate(self, time, value, *args):\n        return _pj_fn(time, value, *args)\n";
    out.script = std::move(src);
    return out;
  }
  // The header declares the backend; the host's inferTransformBackend reads it.
  std::string src = "-- pj-script: " + std::string(language) + "\n";
  src += "local function _pj_make()\n";
  int line = 3;
  src += global_code + "\n";
  if (!global_code.empty()) {
    layout.globals_first_line = line;
    layout.globals_lines = physicalLines(global_code);
  }
  line += physicalLines(global_code);
  src += "  return function(" + params + ")\n";
  ++line;
  layout.body_first_line = line;
  layout.body_lines = physicalLines(body);
  src += body + "\n";
  src += "  end\n";
  src += "end\n";
  src += "local T = { id = \"" + id + "\", name = \"" + name + "\", output = \"double\" }\n";
  src += "T.__index = T\n";
  src += "function T.create(_) return setmetatable({ fn = _pj_make() }, T) end\n";
  src += "function T:calculate(t, v, ...) return self.fn(t, v, ...) end\n";
  src += "return T\n";
  out.script = std::move(src);
  return out;
}

namespace {

std::string userLine(long chunk_line, const ScriptLayout& layout) {
  const auto within = [&](int first, int count) {
    return count > 0 && chunk_line >= first && chunk_line < first + count;
  };
  if (within(layout.body_first_line, layout.body_lines)) {
    return "line " + std::to_string(chunk_line - layout.body_first_line + 1);
  }
  if (within(layout.globals_first_line, layout.globals_lines)) {
    return "globals line " + std::to_string(chunk_line - layout.globals_first_line + 1);
  }
  return "line ?";
}

// The digits at `at`, or 0 digits.
std::size_t digitsAt(const std::string& text, std::size_t at) {
  std::size_t end = at;
  while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
    ++end;
  }
  return end - at;
}

}  // namespace

std::string remapScriptLines(const std::string& error, const ScriptLayout& layout) {
  // Matches `<chunk>:<n>:` at `i` (Luau); returns the index of the closing colon, or 0.
  const auto luau_at = [&](std::size_t i, long& number) -> std::size_t {
    if (i != 0 && isWordChar(error[i - 1])) {
      return 0;
    }
    for (const std::string_view chunk : {"script", "filter", "rule"}) {
      if (error.compare(i, chunk.size(), chunk) != 0 || error.compare(i + chunk.size(), 1, ":") != 0) {
        continue;
      }
      const std::size_t first = i + chunk.size() + 1;
      const std::size_t digits = digitsAt(error, first);
      if (digits != 0 && digits <= 9 && error.compare(first + digits, 1, ":") == 0) {
        number = std::stol(error.substr(first, digits));
        return first + digits;
      }
    }
    return 0;
  };
  // Matches `<string>(<n>)` or `"<string>", line <n>` at `i` (Python); returns the index after it, or 0.
  const auto python_at = [&](std::size_t i, long& number) -> std::size_t {
    std::size_t first = 0;
    std::string_view close;
    if (error.compare(i, 9, "<string>(") == 0) {
      first = i + 9;
      close = ")";
    } else if (error.compare(i, 17, "\"<string>\", line ") == 0) {
      first = i + 17;
    } else {
      return 0;
    }
    const std::size_t digits = digitsAt(error, first);
    if (digits == 0 || digits > 9 || error.compare(first + digits, close.size(), close) != 0) {
      return 0;
    }
    number = std::stol(error.substr(first, digits));
    return first + digits + close.size();
  };
  std::string out;
  std::size_t i = 0;
  while (i < error.size()) {
    long number = 0;
    if (const std::size_t colon = luau_at(i, number); colon != 0) {
      out += userLine(number, layout);
      i = colon;  // the closing colon stays
    } else if (const std::size_t after = python_at(i, number); after != 0) {
      out += userLine(number, layout);
      i = after;
    } else {
      out.push_back(error[i++]);
    }
  }
  return out;
}

bool namesUndefinedName(std::string_view language, std::string_view error) {
  static constexpr std::string_view kLuau[] = {
      "attempt to index nil", "attempt to index a nil value", "attempt to call a nil value"};
  static constexpr std::string_view kPython[] = {"NameError: name '", "'NoneType' object has no attribute"};
  const bool python = language == "python";
  const auto matches = [&](const auto& phrases) {
    return std::any_of(std::begin(phrases), std::end(phrases), [&](std::string_view phrase) {
      return error.find(phrase) != std::string_view::npos;
    });
  };
  return python ? matches(kPython) : matches(kLuau);
}

std::string objectTypeLabel(const std::string& type) {
  if (type == "kSceneEntities") {
    return "scene";
  }
  if (type == "kFrameTransforms") {
    return "transforms";
  }
  // "kOccupancyGrid" -> "occupancy grid": only a builtin object type is split into words.
  const auto parsed = PJ::sdk::parseBuiltinObjectType(type);
  if (!parsed || *parsed == PJ::sdk::BuiltinObjectType::kNone || type.size() < 2 || type.front() != 'k') {
    return type;
  }
  std::string label;
  for (std::size_t i = 1; i < type.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(type[i]);
    if (std::isupper(ch) != 0) {
      if (i > 1) {
        label.push_back(' ');
      }
      label.push_back(static_cast<char>(std::tolower(ch)));
    } else {
      label.push_back(type[i]);
    }
  }
  return label;
}

ParsedOutputs parseTypedOutputs(const nlohmann::json& arr) {
  ParsedOutputs out;
  for (const auto& o : arr) {
    if (!o.is_string()) {
      out.error = "'outputs' entries must be strings \"name:type\"";
      return out;
    }
    const std::string spec = o.get<std::string>();
    const std::size_t colon = spec.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size()) {
      out.error =
          "'outputs' entries must be \"name:type\" (type is 'number', 'string', or a builtin object type "
          "like 'kPointCloud'), got '" +
          spec + "'";
      return out;
    }
    out.outputs.push_back({spec.substr(0, colon), spec.substr(colon + 1)});
  }
  if (out.outputs.empty()) {
    out.error = "'outputs' must declare at least one \"name:type\"";
  }
  return out;
}

bool isObjectOutputType(const std::string& type) {
  const auto parsed = PJ::sdk::parseBuiltinObjectType(type);
  return parsed && *parsed != PJ::sdk::BuiltinObjectType::kNone;
}

std::string sceneKindForOutputType(const std::string& type) {
  const auto parsed = PJ::sdk::parseBuiltinObjectType(type);
  if (parsed &&
      (*parsed == PJ::sdk::BuiltinObjectType::kImage || *parsed == PJ::sdk::BuiltinObjectType::kImageAnnotations ||
       *parsed == PJ::sdk::BuiltinObjectType::kDepthImage || *parsed == PJ::sdk::BuiltinObjectType::kVideoFrame)) {
    return "2d";
  }
  return "3d";
}

// Inverse of toDisplaySeconds: DISPLAY seconds -> raw dataset ns. The SDK
// exposes only the forward direction (toDisplayTimeForSource); this derives
// the same per-source AFFINE offset applyDisplayWindow computes
// (display(raw) = raw*1e-9 + offset, a per-DATASET constant, never a
// per-sample fact) from one forward conversion of a known raw instant — raw
// 0 always qualifies, so no sample lookup is needed — then inverts it.
// Empty when no playback view is bound or the source-scoped conversion is
// not supported by this host.
std::optional<std::int64_t> toRawNs(
    PJ::sdk::PlaybackHostView& playback, PJ::sdk::DataSourceHandle source, double display_s) {
  if (!playback.valid()) {
    return std::nullopt;
  }
  const auto offset_s = playback.toDisplayTimeForSource(source, 0);
  if (!offset_s) {
    return std::nullopt;
  }
  const double raw_s = display_s - *offset_s;
  return static_cast<std::int64_t>(std::llround(raw_s * 1e9));
}

}  // namespace derived_recipes
