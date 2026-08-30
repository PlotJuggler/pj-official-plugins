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

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <ulog_cpp/data_container.hpp>
#include <vector>

namespace ulog_container {

struct TimedParameter {
  ulog_cpp::Parameter parameter;
  uint64_t timestamp_us;
};

class ULogContainer : public ulog_cpp::DataContainer {
 public:
  ULogContainer();

  // Overriding the setter would otherwise hide the base's fileHeader() getter.
  using ulog_cpp::DataContainer::fileHeader;

  void fileHeader(const ulog_cpp::FileHeader& header) override;
  void data(const ulog_cpp::Data& data_msg) override;
  void parameter(const ulog_cpp::Parameter& parameter_msg) override;

  /// Every parameter message seen after the header, in file order, each
  /// stamped with the data-message clock at the time it appeared. Stamps are
  /// >= the file start and non-decreasing per parameter name.
  const std::vector<TimedParameter>& timedChangedParameters() const {
    return timed_changes_;
  }

  /// The initial snapshot with every in-flight change applied in file order:
  /// the LAST known value of each parameter, which is what the original
  /// plugin's Properties dialog showed.
  std::map<std::string, ulog_cpp::Parameter> finalParameters() const;

 private:
  uint64_t file_start_us_{0};
  uint64_t clock_us_{0};
  std::map<uint16_t, std::optional<size_t>> timestamp_offsets_;
  std::map<std::string, uint64_t> last_stamp_us_;
  std::vector<TimedParameter> timed_changes_;
};

}  // namespace ulog_container
