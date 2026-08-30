#pragma once

// SDK-free DataContainer specialisation that gives data-section PARAMETER
// messages a timestamp. The ULog format records parameter *changes* as bare
// key/value messages interleaved with the data stream, with no time of their
// own; the only clock available is the timestamp of the surrounding data
// messages. This container follows that clock and stamps every post-header
// parameter with it, so the importer can plot a parameter's value over the
// flight instead of just its initial snapshot (PlotJuggler#1245).
//
// The clock is deliberately NOT a running maximum: real logs interleave topics
// whose timestamps are only monotonic per subscription, contain zero /
// pre-boot stamps, and occasionally absurd future outliers. So the clock is
// the LAST data timestamp at or after the file start, and each parameter's
// stamp is additionally clamped to that parameter's previous stamp — per-series
// order is what plotting needs, and an outlier only affects changes made while
// it was the most recent data message.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <ulog_cpp/data_container.hpp>
#include <vector>

#include "ulog_flatten.hpp"

namespace ulog_container {

struct TimedParameter {
  ulog_cpp::Parameter parameter;
  uint64_t timestamp_us;
};

class ULogContainer : public ulog_cpp::DataContainer {
 public:
  ULogContainer() : ulog_cpp::DataContainer(StorageConfig::FullLog) {}

  // Overriding the setter would otherwise hide the base's fileHeader() getter.
  using ulog_cpp::DataContainer::fileHeader;

  void fileHeader(const ulog_cpp::FileHeader& header) override {
    ulog_cpp::DataContainer::fileHeader(header);
    // Until the first data message arrives, the file start is the clock, and it
    // stays the floor: the initial parameter snapshot is written there.
    file_start_us_ = header.header().timestamp;
    clock_us_ = file_start_us_;
  }

  void data(const ulog_cpp::Data& data_msg) override {
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

  void parameter(const ulog_cpp::Parameter& parameter_msg) override {
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

  /// Every parameter message seen after the header, in file order, each
  /// stamped with the data-message clock at the time it appeared. Stamps are
  /// >= the file start and non-decreasing per parameter name.
  const std::vector<TimedParameter>& timedChangedParameters() const {
    return timed_changes_;
  }

  /// The initial snapshot with every in-flight change applied in file order:
  /// the LAST known value of each parameter, which is what the original
  /// plugin's Properties dialog showed.
  std::map<std::string, ulog_cpp::Parameter> finalParameters() const {
    auto result = initialParameters();
    for (const auto& change : changedParameters()) {
      result.insert_or_assign(change.field().name(), change);
    }
    return result;
  }

 private:
  uint64_t file_start_us_{0};
  uint64_t clock_us_{0};
  std::map<uint16_t, std::optional<size_t>> timestamp_offsets_;
  std::map<std::string, uint64_t> last_stamp_us_;
  std::vector<TimedParameter> timed_changes_;
};

}  // namespace ulog_container
