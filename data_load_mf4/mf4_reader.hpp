#pragma once

// Thin wrapper over mdflib exposing an MDF file as a flat list of channel groups (topics) whose records stream as
// rows.
//
// Memory: MdfReader::ReadData(dg) materializes a whole *data group*, so readGroup() reads one group, emits its rows
// through a callback, then ClearData()s it before the next: memory is bounded per data group, not per sample.
// Observers live only for one readGroup() call.

#include <mdf/mdfreader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <pj_base/expected.hpp>
#include <pj_base/type_tree.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "mf4_value.hpp"

namespace mf4_detail {

/// `candidate` if not in `used`, otherwise the first free `candidate#1`, `candidate#2`, ...; records the result in
/// `used`. Shared by group-topic and channel-field name de-duplication.
std::string dedupeName(const std::string& candidate, std::unordered_set<std::string>& used);

/// Metadata for one channel of a group.
struct ChannelInfo {
  std::string name;
  std::string unit;
  PJ::PrimitiveType type = PJ::PrimitiveType::kUnspecified;
  bool is_master = false;
};

/// Metadata for one channel group (maps to one PlotJuggler topic).
struct GroupInfo {
  std::size_t dg_index = 0;
  std::string name;  ///< raw MDF channel-group name (may be empty)
  std::uint64_t sample_count = 0;
  int bus_type = 0;  ///< mdf::BusType as int (0 = none, 2 = CAN, ...)
  bool has_master = false;
  std::vector<ChannelInfo> channels;
};

/// One decoded value for a value channel in one record.
struct SampleValue {
  PJ::PrimitiveType type = PJ::PrimitiveType::kFloat64;
  double number = 0.0;  ///< valid when type == kFloat64
  std::string text;     ///< valid when type == kString
  bool valid = true;    ///< false -> null (mdflib reported an invalid sample)
};

/// `ts_ns` is absolute nanoseconds; `values` holds one SampleValue per supported non-master channel, in
/// valueChannelNames() order. The vector is reused across records: consume it synchronously. Return false to stop
/// reading early.
using RowCallback = std::function<bool(std::int64_t ts_ns, const std::vector<SampleValue>& values)>;

/// Per-group read diagnostics.
struct ReadGroupStats {
  /// Samples dropped because the master (time) value was invalid — e.g. the
  /// missing tail of a truncated file, or invalidation bits set by the logger.
  std::uint64_t skipped_invalid_time = 0;
};

/// `ts_ns` is absolute nanoseconds; `bus_channel` is CanMessage::BusChannel() (0 when the file records none);
/// `can_id` is the raw 11/29-bit id. Return false to stop reading early.
using CanFrameCallback = std::function<bool(
    std::int64_t ts_ns, std::uint16_t bus_channel, std::uint32_t can_id, bool extended,
    const std::vector<std::uint8_t>& data)>;

class Mf4Reader {
 public:
  Mf4Reader() = default;
  Mf4Reader(const Mf4Reader&) = delete;
  Mf4Reader& operator=(const Mf4Reader&) = delete;

  /// Reads all metadata blocks, no sample data.
  PJ::Status open(const std::string& path);

  const std::vector<GroupInfo>& groups() const {
    return groups_;
  }
  std::int64_t startTimeNs() const {
    return start_time_ns_;
  }
  bool finalized() const {
    return finalized_;
  }

  /// Supported non-master value channel names, de-duplicated (`name`, then `name#1`, ...; empty names become
  /// `chan{N}`). Parallel to the `values` vector passed to readGroup().
  std::vector<std::string> valueChannelNames(std::size_t group_index) const;

  /// Records whose master (time) value is invalid are skipped and counted in `stats`. Errors if the group has no
  /// master channel or the sample data cannot be read.
  PJ::Status readGroup(std::size_t group_index, const RowCallback& cb, ReadGroupStats* stats = nullptr);

  /// For groups whose bus_type is CAN. Frame time = start time + CanMessage::Timestamp(); frames with an unusable
  /// timestamp are skipped and counted in `stats`.
  PJ::Status readCanGroup(std::size_t group_index, const CanFrameCallback& cb, ReadGroupStats* stats = nullptr);

 private:
  /// Rejects header sample counts that cannot fit in the file (allocation DoS).
  PJ::Status checkSampleCount(std::size_t group_index) const;

  std::unique_ptr<mdf::MdfReader> reader_;
  std::vector<GroupInfo> groups_;
  std::uint64_t file_size_bytes_ = 0;
  std::vector<mdf::IDataGroup*> data_groups_;        // parallel to groups_
  std::vector<mdf::IChannelGroup*> channel_groups_;  // parallel to groups_
  std::int64_t start_time_ns_ = 0;
  bool finalized_ = false;
};

}  // namespace mf4_detail
