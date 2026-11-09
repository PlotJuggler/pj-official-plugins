#include "ulog_container.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "ulog_flatten.hpp"

namespace ulog_container {

namespace {
constexpr int64_t kUnknownOffset = -2;
constexpr int64_t kNoTimestamp = -1;
}  // namespace

ULogContainer::ULogContainer(bool keep_samples)
    : ulog_cpp::DataContainer(StorageConfig::FullLog), keep_samples_(keep_samples) {}

void ULogContainer::fileHeader(const ulog_cpp::FileHeader& header) {
  ulog_cpp::DataContainer::fileHeader(header);
  // Until the first data message arrives, the file start is the clock, and it
  // stays the floor: the initial parameter snapshot is written there.
  file_start_us_ = header.header().timestamp;
  clock_us_ = file_start_us_;
}

void ULogContainer::data(const ulog_cpp::Data& data_msg) {
  if (keep_samples_) {
    // Base stores the sample (and throws on an unknown subscription, which the
    // reader treats as corruption) — let that happen before touching the clock.
    ulog_cpp::DataContainer::data(data_msg);
  }

  // One subscription lookup per msg_id, not per record: ulog_cpp never removes
  // a subscription, so a resolved slot stays valid.
  const size_t id = data_msg.msgId();
  if (id >= timestamp_offsets_.size()) {
    timestamp_offsets_.resize(id + 1, kUnknownOffset);
  }
  int64_t& offset = timestamp_offsets_[id];
  if (offset == kUnknownOffset) {
    const auto& subs = subscriptionsByMessageId();
    const auto sub_it = subs.find(data_msg.msgId());
    if (sub_it == subs.end()) {
      throw ulog_cpp::ParsingException("Invalid subscription");  // same verdict as the base
    }
    offset = kNoTimestamp;
    if (sub_it->second && sub_it->second->format()) {
      if (auto found = ulog_flatten::findTimestampOffset(*sub_it->second->format())) {
        offset = static_cast<int64_t>(*found);
      }
    }
  }

  const auto& raw = data_msg.data();
  if (offset >= 0 && static_cast<size_t>(offset) + sizeof(uint64_t) <= raw.size()) {
    uint64_t ts = 0;
    std::memcpy(&ts, raw.data() + offset, sizeof(ts));
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
