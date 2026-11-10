#include "log_loader.hpp"

#include <cmath>
#include <limits>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/log_codec.hpp>
#include <utility>

namespace log_viewer {

namespace {

// The widest finite range: the host rejects non-finite bounds and saturates finite ones.
constexpr double kMinTime = std::numeric_limits<double>::lowest();
constexpr double kMaxTime = std::numeric_limits<double>::max();

std::string toString(PJ_string_view_t view) {
  return view.data != nullptr && view.size > 0 ? std::string(view.data, view.size) : std::string();
}

Level clampLevel(Level level) {
  return static_cast<uint8_t>(level) <= static_cast<uint8_t>(Level::kFatal) ? level : Level::kUnknown;
}

/// Follows the times of the samples a read delivers.
struct TimeTracker {
  bool any = false;
  double first = 0.0;
  double last = 0.0;
  uint32_t at_last = 0;

  void add(double time) {
    if (!any) {
      any = true;
      first = time;
      last = time;
      at_last = 1;
      return;
    }
    if (time == last) {
      ++at_last;
    } else {
      last = time;
      at_last = 1;
    }
  }
};

}  // namespace

std::string_view logTypeName() {
  return PJ::sdk::name(PJ::sdk::BuiltinObjectType::kLog);
}

SyncResult LogLoader::sync(PJ::Span<const PJ_widget_value_t> values, const HistoryQuery& query) {
  SyncResult result{StoreChange::none(store_.size()), false, {}};

  std::map<uint32_t, std::string> current;
  std::map<uint32_t, bool> unresolved;  // slot -> its value in this update is NONE
  for (const PJ_widget_value_t& value : values) {
    if (value.input != kInput) {
      continue;
    }
    std::string label = toString(value.label);
    if (label.empty()) {
      label = "Topic " + std::to_string(value.slot + 1);
    }
    unresolved[value.slot] = value.kind == PJ_WIDGET_VALUE_NONE;
    current.emplace(value.slot, std::move(label));
  }

  for (auto it = wires_.begin(); it != wires_.end();) {
    const auto now = current.find(it->first);
    if (now == current.end() || now->second != it->second.label) {
      result.change.then(store_.removeWire(it->first));
      result.wires_changed = true;
      it = wires_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [slot, label] : current) {
    if (wires_.find(slot) == wires_.end()) {
      Wire wire;
      wire.label = label;
      wires_.emplace(slot, std::move(wire));
      result.wires_changed = true;
    }
  }

  for (auto& [slot, wire] : wires_) {
    if (!wire.is_log) {
      continue;
    }
    if (!wire.loaded) {
      const auto none = unresolved.find(slot);
      fullLoad(slot, wire, query, result, none != unresolved.end() && none->second);
    } else {
      refresh(slot, wire, query, result);
    }
  }
  return result;
}

PJ::Expected<LogLoader::Probe> LogLoader::probe(uint32_t slot, double to, const HistoryQuery& query) const {
  Probe out;
  auto total = query(kInput, slot, kMinTime, to, 1, [&out](const PJ_widget_value_t& value) {
    out.first_time = value.time;
    if (value.kind == PJ_WIDGET_VALUE_OBJECT) {
      out.first_type = toString(value.type_name);
    }
    return false;
  });
  if (!total) {
    return PJ::unexpected(std::move(total).error());
  }
  out.total = *total;
  return out;
}

void LogLoader::fullLoad(
    uint32_t slot, Wire& wire, const HistoryQuery& query, SyncResult& result, bool value_unresolved) {
  auto counted = probe(slot, kMaxTime, query);
  if (!counted) {
    // A wire whose value is still NONE may not be resolved yet (layout restore): retry on the next update.
    if (!value_unresolved) {
      warnOnce(wire, wire.label + ": cannot read its history: " + counted.error(), result);
    }
    return;
  }
  wire.loaded = true;
  wire.complete = true;
  wire.received = 0;
  wire.host_total = 0;
  if (counted->total == 0) {
    result.change.then(store_.removeWire(slot));
    return;
  }
  if (counted->first_type != logTypeName()) {
    wire.is_log = false;
    result.wires_changed = true;
    warnOnce(wire, wire.label + " is not a log topic", result);
    result.change.then(store_.removeWire(slot));
    return;
  }

  ++full_loads_;
  std::vector<LogRow> rows;
  rows.reserve(counted->total);
  TimeTracker times;
  size_t delivered = 0;
  wire.next_seq = 0;
  auto total = query(kInput, slot, kMinTime, kMaxTime, counted->total, [&](const PJ_widget_value_t& value) {
    ++delivered;
    times.add(value.time);
    LogRow row;
    if (decode(value, wire, row)) {
      rows.push_back(std::move(row));
    }
    return true;
  });
  if (!total) {
    wire.loaded = false;
    warnOnce(wire, wire.label + ": cannot read its history: " + total.error(), result);
    return;
  }
  result.change.then(store_.replaceWire(slot, std::move(rows)));
  wire.host_total = *total;
  wire.host_first = times.first;
  wire.host_last = times.last;
  wire.host_at_last = times.at_last;
  wire.received = delivered;
  wire.complete = delivered >= *total;
}

void LogLoader::refresh(uint32_t slot, Wire& wire, const HistoryQuery& query, SyncResult& result) {
  if (!wire.complete || wire.host_total == 0) {
    // A reduced history cannot be extended sample by sample, and an empty one has nothing to extend.
    auto counted = probe(slot, kMaxTime, query);
    if (counted && counted->total == wire.host_total &&
        (counted->total == 0 || counted->first_time == wire.host_first)) {
      return;
    }
    fullLoad(slot, wire, query, result);
    return;
  }

  auto counted = probe(slot, kMaxTime, query);
  if (!counted) {
    return;
  }
  if (counted->total == wire.host_total && counted->first_time == wire.host_first) {
    return;
  }
  if (counted->total == 0) {
    // The host dropped everything it had: keep the rows already shown.
    wire.host_total = 0;
    return;
  }

  // A later first sample that the widget already holds means the host evicted old samples (streaming
  // buffer). Any other change of the first sample is a display offset change or an earlier insert.
  bool evicted = false;
  if (counted->first_time != wire.host_first) {
    if (counted->first_time > wire.host_first && counted->first_time <= wire.host_last &&
        store_.wireHasTime(slot, counted->first_time)) {
      evicted = true;
    } else {
      fullLoad(slot, wire, query, result);
      return;
    }
  }

  // Samples up to the last one held must be exactly those the widget has: more means a late insert.
  auto up_to_last = probe(slot, wire.host_last, query);
  if (!up_to_last) {
    return;
  }
  const size_t expected = evicted ? store_.wireRowsFrom(slot, counted->first_time) : wire.host_total;
  // The second guard keeps `fresh` below from wrapping if the host changed between the two reads.
  if (up_to_last->total != expected || up_to_last->total > counted->total) {
    fullLoad(slot, wire, query, result);
    return;
  }
  const uint32_t fresh = counted->total - up_to_last->total;
  if (fresh == 0) {
    wire.host_total = counted->total;
    wire.host_first = counted->first_time;
    return;
  }

  // The tail read starts at the last time held, so it repeats the samples at that time first.
  ++incremental_loads_;
  const uint32_t wanted = fresh + wire.host_at_last;
  uint32_t skip = wire.host_at_last;
  const double held_last = wire.host_last;
  std::vector<LogRow> rows;
  rows.reserve(fresh);
  TimeTracker times;
  size_t delivered = 0;
  auto tail = query(kInput, slot, held_last, kMaxTime, wanted, [&](const PJ_widget_value_t& value) {
    if (skip > 0 && value.time == held_last) {
      --skip;
      return true;
    }
    ++delivered;
    times.add(value.time);
    LogRow row;
    if (decode(value, wire, row)) {
      rows.push_back(std::move(row));
    }
    return true;
  });
  if (!tail) {
    return;
  }
  if (*tail != wanted || skip > 0) {
    fullLoad(slot, wire, query, result);
    return;
  }
  result.change.then(store_.insert(slot, std::move(rows)));
  wire.host_total = counted->total;
  wire.host_first = counted->first_time;
  wire.received += delivered;
  if (delivered < fresh) {
    wire.complete = false;
  }
  if (times.any) {
    wire.host_last = times.last;
    wire.host_at_last = times.at_last;
  }
}

void LogLoader::warnOnce(Wire& wire, std::string message, SyncResult& result) {
  if (!wire.warned) {
    wire.warned = true;
    result.warnings.push_back(std::move(message));
  }
}

bool LogLoader::decode(const PJ_widget_value_t& value, Wire& wire, LogRow& row) {
  if (value.kind != PJ_WIDGET_VALUE_OBJECT || toString(value.type_name) != logTypeName() ||
      !std::isfinite(value.time)) {
    return false;
  }
  auto log = PJ::deserializeLog(value.bytes.data, value.bytes.size);
  if (!log) {
    return false;
  }
  row.time = value.time;
  row.stamp_ns = log->timestamp_ns;
  row.seq = wire.next_seq++;
  row.level = clampLevel(log->level);
  row.node = store_.internNode(log->name);
  row.file = store_.intern(log->file);
  row.function = store_.intern(log->function);
  row.line = log->line;
  row.message = std::move(log->message);
  return true;
}

std::vector<WireInfo> LogLoader::wires() const {
  std::vector<WireInfo> out;
  out.reserve(wires_.size());
  for (const auto& [slot, wire] : wires_) {
    out.push_back(WireInfo{slot, wire.label, wire.is_log});
  }
  return out;
}

std::string LogLoader::label(uint32_t wire) const {
  const auto found = wires_.find(wire);
  return found == wires_.end() ? std::string() : found->second.label;
}

size_t LogLoader::receivedSamples() const {
  size_t sum = 0;
  for (const auto& [slot, wire] : wires_) {
    if (wire.is_log) {
      sum += wire.complete ? wire.host_total : wire.received;
    }
  }
  return sum;
}

size_t LogLoader::hostSamples() const {
  size_t sum = 0;
  for (const auto& [slot, wire] : wires_) {
    if (wire.is_log) {
      sum += wire.host_total;
    }
  }
  return sum;
}

}  // namespace log_viewer
