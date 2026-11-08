// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// catalogDigest: the budgeted listing of loaded data that rides along with
// every message, so the model rarely needs list_topics.

#include "catalog_digest.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "derived_recipes/recipes.hpp"

namespace assistant_agent {
namespace {

using derived_recipes::datasetByTopicIndex;

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

// An object topic's time range, in the best axis available: display seconds
// when a playback view is bound and can convert both ends (matching the
// display-time reports of PlotJuggler's own tools), else the raw dataset-domain
// seconds the host reported, flagged so the model never mistakes one axis for
// the other.
struct ObjectTimeRange {
  double a = 0.0;
  double b = 0.0;
  bool raw = false;
};

ObjectTimeRange objectTimeRange(
    const PJ::sdk::PlaybackHostView& playback, PJ_data_source_handle_t source, std::int64_t t_min_ns,
    std::int64_t t_max_ns) {
  if (playback.valid()) {
    const auto display_a = playback.toDisplayTimeForSource(source, t_min_ns);
    const auto display_b = playback.toDisplayTimeForSource(source, t_max_ns);
    if (display_a && display_b) {
      return {*display_a, *display_b, false};
    }
  }
  return {static_cast<double>(t_min_ns) * 1e-9, static_cast<double>(t_max_ns) * 1e-9, true};
}

// How much of a topic's field list catalogDigest rendered, cheapest to
// costliest.
enum class Tier { kCount, kPartial, kFull };

// Plain truncation: the full tree, then (if that still does not fit) topic
// names only, with a footer naming the gap. This is the ENTIRE pre-mixed-tier
// catalogDigest, kept verbatim as the fallback for the rare budget so tight
// that not even a bare field COUNT fits for every topic — see catalogDigest's
// count-only feasibility check below, the only caller.
// Templated over the catalog type so it serves both the legacy (v1) host and
// catalogSnapshotV2()'s scalar half -- both expose the same topics()/fields()/
// dataSources() shape, and this fallback never touches object topics.
template <class Catalog>
std::string legacyCatalogDigest(const Catalog& catalog, std::size_t budget_chars) {
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(catalog.dataSources());

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

// The scalar half of the digest -- everything catalogDigest rendered before
// object topics existed, unchanged in behavior. Templated over the catalog
// type so catalogSnapshotV2()'s scalar arrays (identical shape to v1's) share
// this without a second copy; the caller has already handled "nothing is
// loaded" (that verdict needs to know about object topics too, on a v2 host).
template <class Catalog>
std::string renderScalarCatalogDigest(const Catalog& catalog, std::size_t budget_chars) {
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const std::uint32_t n = static_cast<std::uint32_t>(topics.size());

  // Which dataset each topic belongs to. PJ4 can hold several loaded at once —
  // two runs of the same robot, say — and a flat topic list hides that
  // completely: the model cannot offer to compare them because it does not know
  // there are two, and cannot avoid mixing them for the same reason. Topics are
  // grouped contiguously per source (first_topic/topic_count), so this is a
  // lookup table rather than a scan. Empty when the host reports no sources, in
  // which case the listing stays exactly as it was.
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(catalog.dataSources());

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

  auto topicPrefix = [&](std::uint32_t ti) {
    return "  " + std::string(PJ::sdk::toStringView(topics[ti].name)) + ": ";
  };

  // Sub-budget for a partial line's field list (before the "  <topic>: "
  // prefix and the trailing "… +N more") — enough for a handful of short
  // field names at far less than a full line's cost.
  constexpr std::size_t kPartialFieldBudget = 120;

  auto buildFullBody = [&](std::uint32_t ti) {
    const auto& topic = topics[ti];
    std::string full = topicPrefix(ti);
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
    return full + "\n";
  };
  auto buildCountBody = [&](std::uint32_t ti) {
    return topicPrefix(ti) + std::to_string(topics[ti].field_count) + " fields\n";
  };
  auto buildPartialBody = [&](std::uint32_t ti) {
    const auto& topic = topics[ti];
    const std::string prefix = topicPrefix(ti);
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
    return remaining == 0 ? (prefix + partial_fields + "\n")
                          : (prefix + partial_fields + ", … +" + std::to_string(remaining) + " more\n");
  };

  auto renderHeader = [&] {
    std::string header = "Loaded data (" + std::to_string(n) + " topic(s)):\n";
    // Teach the qualifier by stating it where the dataset names are, instead
    // of spending schema tokens on it in every session: this line exists
    // only when several datasets are actually loaded.
    if (catalog.dataSources().size() >= 2) {
      header +=
          "Several datasets are loaded; when the same topic exists in more than one, address the series as "
          "\"<dataset>:<topic>/<field>\".\n";
    }
    if (dominant_type) {
      header += "fields are " + *dominant_type + " unless marked\n";
    }
    return header;
  };

  // Every topic's full rendering, built up front — the common case (the
  // whole catalog fits with every field spelled out) needs nothing else, so
  // the count and partial tiers below are built lazily, only once that fast
  // path has been ruled out.
  std::vector<std::string> full_body(n);
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    full_body[ti] = buildFullBody(ti);
  }

  {
    std::size_t full_total = 0;
    for (std::uint32_t ti = 0; ti < n; ++ti) {
      full_total += dataset_prefix[ti].size() + full_body[ti].size();
    }
    if (full_total <= budget_chars) {
      std::string body;
      for (std::uint32_t ti = 0; ti < n; ++ti) {
        body += dataset_prefix[ti];
        body += full_body[ti];
      }
      return renderHeader() + body;
    }
  }

  // The full tree doesn't fit whole: every topic's cheapest rendering (a
  // bare field count) is the feasibility floor -- if even THAT doesn't fit,
  // there is nothing left to trim except fields entirely, which is exactly
  // what the pre-existing full-tree/names-only truncation already does.
  std::vector<std::string> count_body(n);
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    count_body[ti] = buildCountBody(ti);
  }
  std::size_t count_total = 0;
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    count_total += dataset_prefix[ti].size() + count_body[ti].size();
  }
  if (count_total > budget_chars) {
    return legacyCatalogDigest(catalog, budget_chars);
  }

  // Every topic starts at "count" (already paid for above) and ascends
  // cheapest-upgrade-first — (full size - count size), stable-sorted so a tie
  // resolves in catalog order — trying full, then partial, stopping the first
  // time an upgrade would blow the budget. Signed deltas: a topic with no
  // fields renders SHORTER as "full" than as "count", and that must read as
  // an improvement, not wrap around as an unsigned underflow. partial_body is
  // filled in lazily, only for the topics this loop actually considers one
  // for (a topic whose full upgrade already fit never needs it).
  std::vector<std::string> partial_body(n);
  std::vector<Tier> tier(n, Tier::kCount);
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
        tier[ti] = Tier::kFull;
        continue;
      }
      partial_body[ti] = buildPartialBody(ti);
      const long long partial_delta = delta(partial_body[ti], count_body[ti]);
      if (total + partial_delta <= budget) {
        total += partial_delta;
        tier[ti] = Tier::kPartial;
      }
    }
  }

  std::string body;
  bool any_reduced = false;
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    body += dataset_prefix[ti];
    switch (tier[ti]) {
      case Tier::kFull:
        body += full_body[ti];
        break;
      case Tier::kPartial:
        body += partial_body[ti];
        any_reduced = true;
        break;
      case Tier::kCount:
        body += count_body[ti];
        any_reduced = true;
        break;
    }
  }

  std::string footer;
  if (any_reduced) {
    footer +=
        "Some topics list only part of their fields (… +N more) or a field count: describe_topic gives the "
        "rest.\n";
  }
  return renderHeader() + body + footer;
}

// One line per non-marker object topic, grouped under the same "dataset
// \"X\":" headers the scalar half uses. Budgeted like a topic with one field:
// every line is a fixed-size unit -- there is no field list inside it to
// shrink -- so unlike a scalar topic it has only one tier, taken whole or
// left out. Lines are appended greedily in catalog order until the next one
// would exceed `budget_chars`; `shown < total` is catalogDigest's signal to
// say the list was cut.
struct ObjectDigestLines {
  std::string body;
  std::size_t shown = 0;
  std::size_t total = 0;
};

ObjectDigestLines renderObjectDigestLines(
    const PJ::sdk::CatalogSnapshotV2& catalog, std::size_t budget_chars, const PJ::sdk::PlaybackHostView& playback) {
  ObjectDigestLines out;
  const bool multi_dataset = catalog.dataSources().size() >= 2;
  std::string current_dataset;
  for (const auto& entry : derived_recipes::listObjectTopics(catalog)) {
    const PJ_object_topic_info_t& obj = entry.info;
    const std::string& name = entry.name;
    const std::string& dataset = entry.dataset;
    ++out.total;
    std::string line;
    if (multi_dataset && dataset != current_dataset) {
      current_dataset = dataset;
      line += "dataset \"" + current_dataset + "\":\n";
    }
    const std::string& type_name = entry.type;
    const ObjectTimeRange range = objectTimeRange(playback, obj.source, obj.time_min_ns, obj.time_max_ns);
    std::ostringstream oss;
    oss << "  " << name << "  [object " << (type_name.empty() ? std::string("unknown") : type_name) << ", "
        << obj.entry_count << " entries, " << std::fixed << std::setprecision(3) << range.a << "-" << range.b
        << (range.raw ? " raw seconds" : " display seconds") << "]\n";
    line += oss.str();
    if (out.body.size() + line.size() > budget_chars) {
      break;
    }
    out.body += line;
    ++out.shown;
  }
  return out;
}

}  // namespace

std::string catalogDigest(
    const PJ::sdk::ToolboxHostView& host, std::size_t budget_chars, const PJ::sdk::PlaybackHostView& playback) {
  auto v2 = host.catalogSnapshotV2();
  if (v2) {
    const ObjectDigestLines object_lines = renderObjectDigestLines(*v2, budget_chars, playback);
    if (v2->topics().empty() && object_lines.total == 0) {
      return "Loaded data: nothing is loaded yet.";
    }
    // Object topics are fixed-size lines with nothing to shrink, so they take
    // their share of the budget first; the scalar tiering algorithm gets
    // whatever is left, which is the whole budget on the common catalog with
    // no object topics -- every existing scalar guarantee is unchanged then.
    const std::size_t scalar_budget =
        budget_chars > object_lines.body.size() ? budget_chars - object_lines.body.size() : 0;
    std::string out = v2->topics().empty() ? std::string() : renderScalarCatalogDigest(*v2, scalar_budget);
    out += object_lines.body;
    if (object_lines.shown < object_lines.total) {
      out += "Some object topics are TRUNCATED above; use list_topics with a filter to find the rest.\n";
    }
    return out;
  }

  auto catalog = host.catalogSnapshot();
  if (!catalog) {
    return "Loaded data: unavailable (" + catalog.error() + "). Use list_topics to look it up.";
  }
  if (catalog->topics().empty()) {
    return "Loaded data: nothing is loaded yet.";
  }
  return renderScalarCatalogDigest(*catalog, budget_chars);
}

}  // namespace assistant_agent
