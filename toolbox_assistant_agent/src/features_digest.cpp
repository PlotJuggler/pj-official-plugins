// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "features_digest.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/text_utils.hpp>
#include <set>
#include <utility>
#include <vector>

#include "fallback_features.hpp"
#include "session_text.hpp"  // kFeaturesListingOpening

namespace assistant_agent {

namespace {

using nlohmann::json;

// The highest document schema (a major version) this renderer understands.
constexpr int kSupportedSchema = 1;

constexpr std::string_view kMarketplace = "File → Extensions Marketplace…";
constexpr std::string_view kNoPanel =
    "No panel of its own: chosen as the message format inside the data source that uses it (for example a stream's "
    "message serialization selector).";
constexpr std::string_view kTruncated = "(list truncated)";

constexpr std::string_view kUnreadableNote =
    "(This PlotJuggler's own feature listing could not be read, so this reference is used instead.)";

// Collapses every run of whitespace or control characters into one space and
// trims: a host string can then never add a line, let alone the blank line
// that would end the block early.
std::string oneLine(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool gap = false;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c <= 0x20 || c == 0x7F) {
      gap = true;
      continue;
    }
    if (gap && !out.empty()) {
      out += ' ';
    }
    gap = false;
    out += ch;
  }
  return out;
}

// A string field, flattened; empty when missing or not a string (unknown).
std::string text(const json& obj, const char* key) {
  const auto it = obj.find(key);
  return it != obj.end() && it->is_string() ? oneLine(it->get_ref<const std::string&>()) : std::string{};
}

// Ends a summary with a full stop unless it already ends a sentence, so the
// next part ("Start: ...") reads as a new one.
std::string sentence(std::string s) {
  if (s.empty()) {
    return s;
  }
  const char last = s.back();
  const bool ended =
      last == '.' || last == '!' || last == '?' || (s.size() >= 3 && std::string_view(s).substr(s.size() - 3) == "…");
  if (!ended) {
    s += '.';
  }
  return s;
}

std::string categoryWords(const std::string& category) {
  if (category == "data_loader") {
    return "data loader";
  }
  if (category == "data_stream") {
    return "streaming source";
  }
  if (category == "message_parser") {
    return "message parser";
  }
  std::string out = category;
  std::replace(out.begin(), out.end(), '_', ' ');
  return out;
}

struct Entry {
  std::string id;
  std::string name;
  std::string version;
  std::string category;
  std::string summary;
  std::string entry_point;
  std::string reason;

  [[nodiscard]] bool isParser() const {
    return category == "message_parser";
  }
  // A parser always gets its no-panel sentence, even with no entry point:
  // without it the model goes looking for the parser's own window.
  [[nodiscard]] std::string parserRoute() const {
    return entry_point.empty() ? std::string(kNoPanel) : entry_point;
  }
};

// nullopt when the list is missing or not an array: unknown, so its section
// is left out rather than rendered as empty.
std::optional<std::vector<Entry>> readEntries(const json& doc, const char* key) {
  const auto it = doc.find(key);
  if (it == doc.end() || !it->is_array()) {
    return std::nullopt;
  }
  std::vector<Entry> out;
  for (const auto& e : *it) {
    if (!e.is_object()) {
      continue;
    }
    Entry entry{text(e, "id"),      text(e, "name"),        text(e, "version"), text(e, "category"),
                text(e, "summary"), text(e, "entry_point"), text(e, "reason")};
    if (entry.name.empty()) {
      entry.name = entry.id;
    }
    out.push_back(std::move(entry));
  }
  return out;
}

// Name order (case-insensitive, id as tie-break) and the precedence the
// document promises (loaded > installed_not_loaded > available): an id
// already rendered in a higher list is skipped, should a host ever repeat it.
void sortAndDedup(std::vector<Entry>& list, std::set<std::string>& seen) {
  std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
    const std::string la = PJ::sdk::lowerAscii(a.name);
    const std::string lb = PJ::sdk::lowerAscii(b.name);
    return la != lb ? la < lb : a.id < b.id;
  });
  std::vector<Entry> kept;
  for (auto& e : list) {
    if (!e.id.empty() && !seen.insert(e.id).second) {
      continue;
    }
    kept.push_back(std::move(e));
  }
  list = std::move(kept);
}

std::string pluginHead(const Entry& e, bool with_version) {
  std::string out = "- " + e.name;
  if (with_version && !e.version.empty()) {
    out += " " + e.version;
  }
  if (!e.category.empty()) {
    out += " (" + categoryWords(e.category) + ")";
  }
  return out + ":";
}

void append(std::string& line, const std::string& part) {
  if (!part.empty()) {
    line += " " + part;
  }
}

// " Start: <entry point>" style tail, empty without an entry point.
std::string startSuffix(const Entry& e, std::string_view label) {
  return e.entry_point.empty() ? std::string{} : std::string(label) + e.entry_point;
}

// A parser always gets its route sentence; any other entry its entry point
// behind `label`.
std::string routeSuffix(const Entry& e, std::string_view label) {
  return e.isParser() ? e.parserRoute() : startSuffix(e, label);
}

std::string coreLine(const Entry& e) {
  std::string line = "- " + e.name + ":";
  append(line, sentence(e.summary));
  append(line, startSuffix(e, "Start: "));
  return line;
}

std::string loadedLine(const Entry& e) {
  std::string line = pluginHead(e, true);
  append(line, sentence(e.summary));
  append(line, routeSuffix(e, "Start: "));
  return line;
}

std::string installedLine(const Entry& e) {
  std::string line = pluginHead(e, true);
  append(line, sentence(e.summary));
  bool can_start = true;
  if (e.reason == "disabled") {
    append(line, "Not active: enable it in " + std::string(kMarketplace) + " and restart PlotJuggler.");
  } else if (e.reason == "needs_restart") {
    append(line, "Not active: restart PlotJuggler.");
  } else if (e.reason == "incompatible") {
    append(line, "Not active: not compatible with this PlotJuggler version.");
    can_start = false;
  } else {
    append(line, "Not active (reason not reported).");
    can_start = false;
  }
  if (can_start) {
    append(line, routeSuffix(e, "Then: "));
  }
  return line;
}

std::string availableLine(const Entry& e, bool with_summary) {
  std::string line = pluginHead(e, with_summary);
  if (with_summary) {
    append(line, sentence(e.summary));
  }
  std::string install = "Install: " + std::string(kMarketplace) + " → search \"" + e.name + "\" → Install";
  if (e.isParser()) {
    install += ". " + e.parserRoute();
  } else if (!e.entry_point.empty()) {
    install += "; after installing: " + e.entry_point;
  }
  append(line, install);
  return line;
}

std::string registryStateWords(const std::string& state) {
  if (state == "pending") {
    return "it was still loading";
  }
  if (state == "unreachable") {
    return "the Marketplace server could not be reached";
  }
  if (state == "not_fetched") {
    return "it was not fetched in this session";
  }
  return "its state was not reported";
}

std::string registryReasonWords(const std::string& reason) {
  if (reason == "updates_disabled") {
    return "update checks are turned off";
  }
  if (reason == "screenshot") {
    return "PlotJuggler runs in screenshot mode";
  }
  if (reason == "url_changed") {
    return "the Marketplace address changed and the list was not reloaded yet";
  }
  if (reason == "network") {
    return "network error";
  }
  return {};
}

// Whether the Marketplace list was fetched, and the line saying so when not.
std::pair<bool, std::string> registryLine(const json& doc) {
  std::string state;
  std::string reason;
  const auto it = doc.find("registry");
  if (it != doc.end() && it->is_object()) {
    state = text(*it, "state");
    reason = text(*it, "reason");
  }
  if (state == "fetched") {
    return {true, {}};
  }
  std::string words = registryStateWords(state);
  const std::string reason_words = registryReasonWords(reason);
  if (!reason_words.empty()) {
    words += ": " + reason_words;
  }
  return {
      false, "The Marketplace list could not be checked (" + words + "); more plugins may exist: see " +
                 std::string(kMarketplace)};
}

std::size_t textSize(const std::vector<std::string>& lines) {
  std::size_t n = 0;
  for (const auto& l : lines) {
    n += l.size() + 1;
  }
  return n;
}

std::string joinLines(const std::vector<std::string>& lines) {
  std::string out;
  out.reserve(textSize(lines));
  for (const auto& l : lines) {
    if (!out.empty()) {
      out += '\n';
    }
    out += l;
  }
  return out;
}

void addSection(std::vector<std::string>& out, const char* heading, const std::vector<std::string>& lines) {
  if (lines.empty()) {
    out.push_back(std::string(heading) + " none");
    return;
  }
  out.emplace_back(heading);
  out.insert(out.end(), lines.begin(), lines.end());
}

// One section of the listing from the document list `key`. A missing list
// leaves the section out. `line` renders an entry, or returns an empty string
// to skip it. With `seen` the list is first sorted and deduplicated against
// the ids already rendered above it.
template <typename LineFn>
void addListSection(
    std::vector<std::string>& out, const json& doc, const char* key, const char* heading, LineFn line,
    std::set<std::string>* seen = nullptr) {
  auto entries = readEntries(doc, key);
  if (!entries) {
    return;
  }
  if (seen != nullptr) {
    sortAndDedup(*entries, *seen);
  }
  std::vector<std::string> lines;
  for (const auto& e : *entries) {
    if (std::string l = line(e); !l.empty()) {
      lines.push_back(std::move(l));
    }
  }
  addSection(out, heading, lines);
}

}  // namespace

std::string featuresDigest(std::string_view json_text, std::size_t budget_chars) {
  const json doc = json::parse(json_text.begin(), json_text.end(), nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) {
    return fallbackFeatures(kUnreadableNote);
  }
  const auto schema_it = doc.find("schema");
  if (schema_it == doc.end() || !schema_it->is_number_integer() || schema_it->get<long long>() < 1) {
    return fallbackFeatures(kUnreadableNote);
  }
  if (const long long schema = schema_it->get<long long>(); schema > kSupportedSchema) {
    return fallbackFeatures(
        "(This PlotJuggler's own feature listing uses schema " + std::to_string(schema) +
        ", newer than this assistant reads, so this reference is used instead.)");
  }

  const std::string host_version = text(doc, "host_version");
  std::string header(kFeaturesListingOpening);
  header += host_version.empty() ? " (from the host):" : " (PlotJuggler " + host_version + ", from the host):";

  // Everything that is never dropped: built in, limitations, and what is
  // installed (loaded or not).
  std::vector<std::string> fixed;
  std::set<std::string> seen;
  addListSection(
      fixed, doc, "core", "Built in:", [](const Entry& e) { return e.name.empty() ? std::string{} : coreLine(e); });
  addListSection(fixed, doc, "limitations", "Known limitations:", [](const Entry& e) {
    return e.summary.empty() ? std::string{} : "- " + sentence(e.summary);
  });
  addListSection(fixed, doc, "loaded", "Plugins loaded (installed and ready to use):", loadedLine, &seen);
  addListSection(fixed, doc, "installed_not_loaded", "Installed but not active:", installedLine, &seen);

  const auto registry = registryLine(doc);
  const bool fetched = registry.first;
  const std::string& registry_line = registry.second;
  std::vector<Entry> available;
  bool show_available = false;
  if (auto list = readEntries(doc, "available")) {
    sortAndDedup(*list, seen);
    available = std::move(*list);
    // An empty list says "none" only when the Marketplace was actually
    // checked; otherwise the registry line below speaks for it.
    show_available = fetched || !available.empty();
  }
  constexpr const char* kAvailableHeading = "Available in the Marketplace (not installed):";

  std::vector<std::string> footer;
  if (!registry_line.empty()) {
    footer.push_back(registry_line);
  }
  footer.push_back(
      "Plugins are installed from " + std::string(kMarketplace) +
      "; a plugin not listed here may still be distributed outside the Marketplace.");

  const auto render = [&](const std::vector<std::string>& available_lines) {
    std::vector<std::string> all{header};
    all.insert(all.end(), fixed.begin(), fixed.end());
    all.insert(all.end(), available_lines.begin(), available_lines.end());
    all.insert(all.end(), footer.begin(), footer.end());
    return all;
  };
  const auto availableSection = [&](bool with_summary, std::size_t keep) {
    std::vector<std::string> lines;
    if (!show_available) {
      return lines;
    }
    std::vector<std::string> entries;
    for (std::size_t i = 0; i < keep; ++i) {
      entries.push_back(availableLine(available[i], with_summary));
    }
    if (keep == 0 && !available.empty()) {
      lines.emplace_back(kAvailableHeading);  // every entry dropped: the count below, not "none"
    } else {
      addSection(lines, kAvailableHeading, entries);
    }
    if (keep < available.size()) {
      lines.push_back("(" + std::to_string(available.size() - keep) + " more plugins not shown; see the Marketplace)");
    }
    return lines;
  };

  // 1. Everything. 2. Marketplace entries without their summaries.
  for (const bool with_summary : {true, false}) {
    const auto all = render(availableSection(with_summary, available.size()));
    if (textSize(all) <= budget_chars + 1) {
      return joinLines(all);
    }
  }
  // 3. Marketplace entries dropped from the end, counted.
  for (std::size_t keep = available.size(); keep-- > 0;) {
    const auto all = render(availableSection(false, keep));
    if (textSize(all) <= budget_chars + 1) {
      return joinLines(all);
    }
  }
  // 4. What is never dropped silently does not fit on its own: cut it, and
  //    say so, keeping the header and the footer.
  std::vector<std::string> all{header};
  std::size_t used = textSize(all) + textSize(footer) + kTruncated.size() + 1;
  for (const auto& line : fixed) {
    if (used + line.size() + 1 > budget_chars + 1) {
      break;
    }
    used += line.size() + 1;
    all.push_back(line);
  }
  all.emplace_back(kTruncated);
  all.insert(all.end(), footer.begin(), footer.end());
  return joinLines(all);
}

std::string featuresBlock(const PJ::sdk::FeatureCatalogView& view, std::size_t budget_chars) {
  if (!view.valid()) {
    return fallbackFeatures();
  }
  const auto doc = view.json();
  if (!doc) {
    return fallbackFeatures(kUnreadableNote);
  }
  return featuresDigest(*doc, budget_chars);
}

}  // namespace assistant_agent
