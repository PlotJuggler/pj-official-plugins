#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <pj_base/number_parse.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "influx_manifest.hpp"

namespace {

constexpr auto npos = std::string_view::npos;

/// Characters a backslash escapes in a measurement, tag or field key.
constexpr std::string_view kKeyEscapes = ",= ";
/// Characters a backslash escapes inside a quoted string field value.
constexpr std::string_view kStringEscapes = "\"\\";

std::string_view trim(std::string_view text) {
  const size_t first = text.find_first_not_of(" \t\r");
  if (first == npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

/// Index of the first `sep` that no backslash escapes and, when `quotes` is
/// set, that is not inside a double-quoted string value; npos if none. A
/// string value opens only right after '=': a quote anywhere else is literal.
size_t findUnescaped(std::string_view text, char sep, bool quotes) {
  bool in_quotes = false;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\') {
      ++i;
    } else if (quotes && text[i] == '"' && (in_quotes || (i > 0 && text[i - 1] == '='))) {
      in_quotes = !in_quotes;
    } else if (text[i] == sep && !in_quotes) {
      return i;
    }
  }
  return npos;
}

/// Splits `text` at every separator findUnescaped would find, dropping empty parts.
std::vector<std::string_view> splitUnescaped(std::string_view text, char sep, bool quotes) {
  std::vector<std::string_view> parts;
  while (!text.empty()) {
    const size_t end = findUnescaped(text, sep, quotes);
    if (end != 0) {
      parts.push_back(text.substr(0, end));
    }
    if (end == npos) {
      break;
    }
    text.remove_prefix(end + 1);
  }
  return parts;
}

/// Drops the backslash before each character in `escapable`; any other backslash is literal.
std::string unescape(std::string_view text, std::string_view escapable) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size() && escapable.find(text[i + 1]) != npos) {
      ++i;
    }
    out += text[i];
  }
  return out;
}

template <typename T>
std::optional<PJ::sdk::ValueRef> parseAs(std::string_view text) {
  if (auto value = PJ::parseNumber<T>(text)) {
    return PJ::sdk::ValueRef{*value};
  }
  return std::nullopt;
}

/// Decodes one field value: "quoted" string, boolean, 12i integer, 12u
/// unsigned or float. nullopt when it is none of these. A string value is
/// stored in `texts`, which must outlive the returned view.
std::optional<PJ::sdk::ValueRef> parseValue(std::string_view text, std::deque<std::string>& texts) {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    const std::string& value = texts.emplace_back(unescape(text.substr(1, text.size() - 2), kStringEscapes));
    return PJ::sdk::ValueRef{std::string_view(value)};
  }
  if (text == "t" || text == "T" || text == "true" || text == "True" || text == "TRUE") {
    return PJ::sdk::ValueRef{true};
  }
  if (text == "f" || text == "F" || text == "false" || text == "False" || text == "FALSE") {
    return PJ::sdk::ValueRef{false};
  }
  if (text.ends_with('i')) {
    return parseAs<int64_t>(text.substr(0, text.size() - 1));
  }
  if (text.ends_with('u')) {
    return parseAs<uint64_t>(text.substr(0, text.size() - 1));
  }
  return parseAs<double>(text);
}

/// `name` as the host stores it: runs of '/' collapsed, no leading '/'. Names
/// are compared in this form, since the host rejects a row whose names collide
/// only after it (e.g. "m//a" and "m/a").
std::string hostSeriesName(std::string name) {
  name.erase(std::unique(name.begin(), name.end(), [](char a, char b) { return a == '/' && b == '/'; }), name.end());
  if (name.starts_with('/')) {
    name.erase(0, 1);
  }
  return name;
}

/// The field named `name` in `fields`, or fields.end().
auto findField(auto& fields, std::string_view name) {
  return std::find_if(fields.begin(), fields.end(), [name](const auto& field) { return field.name == name; });
}

/// Parses InfluxDB line protocol, one or more lines per message:
///
///   measurement[,tag=value...] field=value[,field=value...] [timestamp_ns]
///
/// Each field becomes the series "measurement/tag=value/.../field". A line
/// without a timestamp takes the message receive time. Malformed lines and
/// fields are skipped, so one bad line never drops the rest of the message.
class InfluxParser : public PJ::MessageParserPluginBase {
 public:
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("write host not bound"));
    }
    // row_ is empty here unless an exception escaped the last message mid-row;
    // its views would then point into the texts_ cleared below.
    row_.clear();
    texts_.clear();
    std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
    while (!text.empty()) {
      const size_t end = text.find('\n');
      if (auto status = parseLine(trim(text.substr(0, end)), timestamp_ns); !status) {
        return status;
      }
      text.remove_prefix(end == npos ? text.size() : end + 1);
    }
    return flushRow();
  }

 private:
  PJ::Status parseLine(std::string_view line, PJ::Timestamp receive_time) {
    if (line.empty() || line.front() == '#') {
      return PJ::okStatus();
    }
    const size_t series_end = findUnescaped(line, ' ', false);
    if (series_end == npos) {
      return PJ::okStatus();
    }
    const std::string_view rest = trim(line.substr(series_end + 1));
    const size_t fields_end = findUnescaped(rest, ' ', true);
    PJ::Timestamp ts = receive_time;
    if (fields_end != npos) {
      // A malformed timestamp, or a fourth section, invalidates the line.
      const auto stamp = PJ::parseNumber<int64_t>(trim(rest.substr(fields_end + 1)));
      if (!stamp) {
        return PJ::okStatus();
      }
      ts = *stamp;
    }

    std::string prefix;
    for (const auto part : splitUnescaped(line.substr(0, series_end), ',', false)) {
      prefix += unescape(part, kKeyEscapes);
      prefix += '/';
    }

    // The host rejects a row that names a series twice, so a repeated field
    // key keeps its last value.
    std::vector<PJ::sdk::NamedFieldValue> fields;
    for (const auto field : splitUnescaped(rest.substr(0, fields_end), ',', true)) {
      const size_t eq = findUnescaped(field, '=', false);
      if (eq == npos || eq == 0) {
        continue;
      }
      auto value = parseValue(field.substr(eq + 1), texts_);
      if (!value) {
        continue;
      }
      std::string name = hostSeriesName(prefix + unescape(field.substr(0, eq), kKeyEscapes));
      if (auto same = findField(fields, name); same != fields.end()) {
        same->value = *value;
      } else {
        fields.push_back({std::move(name), *value});
      }
    }
    if (fields.empty()) {
      return PJ::okStatus();
    }

    // Lines sharing a timestamp share a row (Telegraf stamps every metric of
    // one collection alike), until one repeats a series already in it.
    // ponytail: O(row x line) name scan, fine for a collection's tens to hundreds
    // of series; past ~1k series under one timestamp (e.g. a big MQTT message
    // whose lines carry no timestamp) index the row's names.
    const auto repeats = [this](const auto& field) { return findField(row_, field.name) != row_.end(); };
    if (!row_.empty() && (ts != row_ts_ || std::any_of(fields.begin(), fields.end(), repeats))) {
      if (auto status = flushRow(); !status) {
        return status;
      }
    }
    row_ts_ = ts;
    row_.insert(row_.end(), std::make_move_iterator(fields.begin()), std::make_move_iterator(fields.end()));
    return PJ::okStatus();
  }

  PJ::Status flushRow() {
    if (row_.empty()) {
      return PJ::okStatus();
    }
    auto status = writeHost().appendRecord(row_ts_, PJ::Span<const PJ::sdk::NamedFieldValue>(row_.data(), row_.size()));
    row_.clear();
    return status;
  }

  std::vector<PJ::sdk::NamedFieldValue> row_;  ///< fields of the row being built; empty between messages
  PJ::Timestamp row_ts_ = 0;
  std::deque<std::string> texts_;  ///< string values of this message; a deque keeps their addresses stable
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(InfluxParser, kInfluxManifest)
