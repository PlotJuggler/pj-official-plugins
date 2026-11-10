#include "log_store.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace log_viewer {

namespace {

/// Merge order: time, then wire, then arrival within the wire.
bool before(const LogRow& a, const LogRow& b) {
  if (a.time != b.time) {
    return a.time < b.time;
  }
  if (a.wire != b.wire) {
    return a.wire < b.wire;
  }
  return a.seq < b.seq;
}

}  // namespace

void StoreChange::then(const StoreChange& next) {
  if (reset || next.reset) {
    reset = true;
    return;
  }
  // `next` is expressed against the state after this change, where the unchanged rows are
  // [0, old_tail - dropped_front); its own unchanged range starts at its dropped_front.
  old_tail = std::min(old_tail, next.old_tail + dropped_front);
  dropped_front += next.dropped_front;
}

uint32_t LogStore::intern(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  const auto found = string_ids_.find(text);
  if (found != string_ids_.end()) {
    return found->second;
  }
  const auto id = static_cast<uint32_t>(strings_.size());
  strings_.emplace_back(text);
  string_ids_.emplace(strings_.back(), id);
  is_node_.push_back(false);
  return id;
}

uint32_t LogStore::internNode(std::string_view text) {
  const uint32_t id = intern(text);
  if (id != 0 && !is_node_[id]) {
    is_node_[id] = true;
    nodes_.push_back(id);
  }
  return id;
}

std::optional<uint32_t> LogStore::find(std::string_view text) const {
  if (text.empty()) {
    return 0U;
  }
  const auto found = string_ids_.find(text);
  if (found == string_ids_.end()) {
    return std::nullopt;
  }
  return found->second;
}

StoreChange LogStore::insert(uint32_t wire, std::vector<LogRow> rows) {
  StoreChange change = StoreChange::none(rows_.size());
  if (rows.empty()) {
    return change;
  }
  auto& times = wire_times_[wire];
  for (auto& row : rows) {
    row.wire = wire;
    times.push_back(row.time);
  }
  const auto first = std::lower_bound(rows_.begin(), rows_.end(), rows.front(), before);
  const auto position = static_cast<size_t>(std::distance(rows_.begin(), first));
  change.old_tail = position;
  if (position == rows_.size()) {
    std::move(rows.begin(), rows.end(), std::back_inserter(rows_));
  } else {
    // Merge the new rows with the tail they interleave with; the head stays where it is.
    std::vector<LogRow> tail(std::make_move_iterator(first), std::make_move_iterator(rows_.end()));
    rows_.erase(first, rows_.end());
    std::merge(
        std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()),
        std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()), std::back_inserter(rows_), before);
  }
  evictOverflow(change);
  return change;
}

StoreChange LogStore::removeWire(uint32_t wire) {
  StoreChange change = StoreChange::none(rows_.size());
  const auto times = wire_times_.find(wire);
  if (times == wire_times_.end()) {
    return change;
  }
  if (!times->second.empty()) {
    LogRow probe;
    probe.time = times->second.front();
    probe.wire = wire;
    probe.seq = 0;
    const auto first = std::lower_bound(rows_.begin(), rows_.end(), probe, before);
    change.old_tail = static_cast<size_t>(std::distance(rows_.begin(), first));
    const auto kept = std::remove_if(first, rows_.end(), [wire](const LogRow& row) { return row.wire == wire; });
    rows_.erase(kept, rows_.end());
  }
  wire_times_.erase(times);
  return change;
}

StoreChange LogStore::replaceWire(uint32_t wire, std::vector<LogRow> rows) {
  StoreChange change = removeWire(wire);
  change.then(insert(wire, std::move(rows)));
  return change;
}

bool LogStore::wireHasTime(uint32_t wire, double time) const {
  const auto times = wire_times_.find(wire);
  return times != wire_times_.end() && std::binary_search(times->second.begin(), times->second.end(), time);
}

size_t LogStore::wireRowsFrom(uint32_t wire, double time) const {
  const auto times = wire_times_.find(wire);
  if (times == wire_times_.end()) {
    return 0;
  }
  const auto first = std::lower_bound(times->second.begin(), times->second.end(), time);
  return static_cast<size_t>(std::distance(first, times->second.end()));
}

void LogStore::evictOverflow(StoreChange& change) {
  while (rows_.size() > capacity_) {
    auto times = wire_times_.find(rows_.front().wire);
    if (times != wire_times_.end() && !times->second.empty()) {
      times->second.pop_front();
    }
    rows_.pop_front();
    ++change.dropped_front;
  }
}

}  // namespace log_viewer
