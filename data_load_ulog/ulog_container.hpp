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
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <ulog_cpp/data_container.hpp>
#include <ulog_cpp/reader.hpp>
#include <vector>

namespace ulog_container {

/// The largest ULog timestamp (microseconds) whose nanoseconds fit in int64 (about 292 years). Anything above is a
/// damaged value: converting it would overflow.
inline constexpr uint64_t kMaxTimestampUs = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / 1000;

struct TimedParameter {
  ulog_cpp::Parameter parameter;
  uint64_t timestamp_us;
};

class ULogContainer : public ulog_cpp::DataContainer {
 public:
  /// `keep_samples = false` parses everything except the data records
  /// themselves (the parameter clock still follows them): for readers that only
  /// want the header, parameters and log messages.
  explicit ULogContainer(bool keep_samples = true);

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
  /// the LAST known value of each parameter (the dataset metadata's `parameters`).
  std::map<std::string, ulog_cpp::Parameter> finalParameters() const;

 private:
  uint64_t file_start_us_{0};
  uint64_t clock_us_{0};
  bool keep_samples_;
  // Indexed by msg_id; kUnknownOffset until first seen, kNoTimestamp if absent.
  std::vector<int64_t> timestamp_offsets_;
  std::map<std::string, uint64_t> last_stamp_us_;
  std::vector<TimedParameter> timed_changes_;
};

struct ReadOutcome {
  bool opened = false;
  /// Set when ulog_cpp threw while reading (some damaged messages raise more than its ParsingException): the read
  /// stopped there, and everything before it is in the container.
  std::string error;
};

/// Reads `path` (UTF-8, as the host passes it) through `container` in 64 KiB chunks. `after_chunk(bytes_read)` returns
/// false to stop early.
template <typename AfterChunk>
ReadOutcome readLog(
    const std::string& path, const std::shared_ptr<ulog_cpp::DataContainer>& container, AfterChunk&& after_chunk) {
  // A narrow path is read in the ANSI code page on Windows; a char8_t path is not.
  std::ifstream file(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  ReadOutcome outcome;
  if (!file.is_open()) {
    return outcome;
  }
  outcome.opened = true;
  ulog_cpp::Reader reader{container};
  static constexpr size_t kChunkSize = 65536;
  std::vector<uint8_t> buffer(kChunkSize);
  uint64_t bytes_read = 0;
  while (file) {
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(kChunkSize));
    auto count = static_cast<size_t>(file.gcount());
    if (count == 0) {
      break;
    }
    try {
      reader.readChunk(buffer.data(), static_cast<int>(count));
    } catch (const std::exception& e) {
      outcome.error = e.what();
      break;
    }
    bytes_read += count;
    if (!after_chunk(bytes_read)) {
      break;
    }
  }
  return outcome;
}

}  // namespace ulog_container
