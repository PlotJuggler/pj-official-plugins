#include "ulog_container.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "ulog_flatten.hpp"

namespace ulog_container {

ULogContainer::ULogContainer() : ulog_cpp::DataContainer(StorageConfig::FullLog) {}

void ULogContainer::fileHeader(const ulog_cpp::FileHeader& header) {
  ulog_cpp::DataContainer::fileHeader(header);
  // Until the first data message arrives, the file start is the clock, and it
  // stays the floor: the initial parameter snapshot is written there.
  file_start_us_ = header.header().timestamp;
  clock_us_ = file_start_us_;
}

void ULogContainer::data(const ulog_cpp::Data& data_msg) {
  // Base stores the sample (and throws on an unknown subscription, which the
  // reader treats as corruption) — let that happen before touching the clock.
  ulog_cpp::DataContainer::data(data_msg);

  auto it = timestamp_offsets_.find(data_msg.msgId());
  if (it == timestamp_offsets_.end()) {
    std::optional<size_t> offset;
    const auto& subs = subscriptionsByMessageId();
    auto sub_it = subs.find(data_msg.msgId());
    if (sub_it != subs.end() && sub_it->second && sub_it->second->format()) {
      offset = ulog_flatten::findTimestampOffset(*sub_it->second->format());
    }
    it = timestamp_offsets_.emplace(data_msg.msgId(), offset).first;
  }

  const auto& raw = data_msg.data();
  if (it->second && *it->second + sizeof(uint64_t) <= raw.size()) {
    uint64_t ts = 0;
    std::memcpy(&ts, raw.data() + *it->second, sizeof(ts));
    if (ts >= file_start_us_) {
      clock_us_ = ts;
    }
  }
}

void ULogContainer::parameter(const ulog_cpp::Parameter& parameter_msg) {
  ulog_cpp::DataContainer::parameter(parameter_msg);
  if (!isHeaderComplete()) {
    return;  // initial snapshot: base keeps it in initialParameters()
  }
  TimedParameter timed{parameter_msg, clock_us_};
  // Post-header fields are never resolved by headerComplete(); resolve here so
  // value() can be read (same as the base does for its own changed copy).
  timed.parameter.field().resolveDefinition(messageFormats(), 0);

  uint64_t& last = last_stamp_us_[timed.parameter.field().name()];
  timed.timestamp_us = std::max(timed.timestamp_us, last);
  last = timed.timestamp_us;
  timed_changes_.push_back(std::move(timed));
}

std::map<std::string, ulog_cpp::Parameter> ULogContainer::finalParameters() const {
  auto result = initialParameters();
  for (const auto& change : changedParameters()) {
    result.insert_or_assign(change.field().name(), change);
  }
  return result;
}

}  // namespace ulog_container
