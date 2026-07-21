#include "blf_frames.hpp"

#include <blf_reader.hh>
#include <blf_structs.hh>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>

namespace blf_detail {

namespace {

/// DBC/BLF extended-frame flag: bit 31 of the CAN id.
constexpr std::uint32_t kExtendedFlag = 0x8000'0000u;
constexpr std::uint32_t kCanIdMask = 0x1FFF'FFFFu;
/// ObjectFlags_e::TimeTenMics — timestamp is in 10-microsecond ticks (else ns).
constexpr std::uint32_t kTimeTenMics = 0x1u;

/// Windows SYSTEMTIME -> nanoseconds since the Unix epoch (treated as UTC).
std::int64_t sysTimeToNs(const lblf::blf_struct::sysTime_t& st) {
  // A far-future year (the field is file-controlled, e.g. 32767) overflows the
  // nanosecond duration_cast; int64 ns only spans ~year 1678..2262. Bound the
  // year so a corrupt header yields epoch 0, not undefined arithmetic.
  if (st.year == 0 || st.year < 1678 || st.year > 2262) {
    return 0;
  }
  using namespace std::chrono;
  const year_month_day ymd{
      year{static_cast<int>(st.year)}, month{static_cast<unsigned>(st.month)}, day{static_cast<unsigned>(st.day)}};
  if (!ymd.ok()) {
    return 0;
  }
  const sys_days days{ymd};
  const auto tp = days + hours{st.hour} + minutes{st.minute} + seconds{st.second} + milliseconds{st.milliseconds};
  return duration_cast<nanoseconds>(tp.time_since_epoch()).count();
}

/// start + rel without signed overflow (clamps to the int64 range).
std::int64_t saturatingAddNs(std::int64_t start, std::int64_t rel) {
  if (rel > 0 && start > std::numeric_limits<std::int64_t>::max() - rel) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (rel < 0 && start < std::numeric_limits<std::int64_t>::min() - rel) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return start + rel;
}

template <typename CanMsg>
CanFrame toFrame(const CanMsg& can, std::int64_t start_ns) {
  CanFrame frame;
  frame.ts_ns =
      saturatingAddNs(start_ns, objectTimeNs(static_cast<std::uint32_t>(can.obh.objectFlags), can.obh.objectTimeStamp));
  frame.channel = can.channel;
  frame.can_id = can.id & kCanIdMask;
  frame.extended = (can.id & kExtendedFlag) != 0;
  const std::size_t n =
      static_cast<std::size_t>(can.dlc) < can.data.size() ? static_cast<std::size_t>(can.dlc) : can.data.size();
  frame.data.assign(can.data.begin(), can.data.begin() + static_cast<std::ptrdiff_t>(n));
  return frame;
}

}  // namespace

PJ::Status readCanFrames(const std::string& path, const CanFrameCallback& cb, BlfStats& stats) {
  stats = {};
  try {
    lblf::blf_reader reader(path);
    // Filled before the first callback so consumers can report progress
    // against it while stats.can_frames / skipped_objects advance live.
    stats.total_objects = reader.getfileStatistics().objCount;
    const std::int64_t start_ns = sysTimeToNs(reader.getfileStatistics().meas_start_time);
    bool keep_going = true;
    while (keep_going && reader.next()) {
      const auto data = reader.data();
      switch (data.base_header.objectType) {
        case lblf::ObjectType_e::CAN_MESSAGE: {
          lblf::blf_struct::CanMessage_obh can{};
          lblf::read_blf_struct(data, can);
          keep_going = cb(toFrame(can, start_ns));
          ++stats.can_frames;
          break;
        }
        case lblf::ObjectType_e::CAN_MESSAGE2: {
          lblf::blf_struct::CanMessage2_obh can{};
          lblf::read_blf_struct(data, can);
          keep_going = cb(toFrame(can, start_ns));
          ++stats.can_frames;
          break;
        }
        default:
          ++stats.skipped_objects;
          break;
      }
    }
  } catch (const std::exception& err) {
    return PJ::unexpected(std::string("blf: cannot read ") + path + ": " + err.what());
  }
  return PJ::okStatus();
}

std::int64_t objectTimeNs(std::uint32_t object_flags, std::uint64_t ticks) {
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if ((object_flags & kTimeTenMics) != 0) {
    if (ticks > kMax / 10000) {
      return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(ticks * 10000);
  }
  if (ticks > kMax) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(ticks);
}

std::optional<std::uint16_t> parseChannelKey(std::string_view key) {
  std::uint16_t channel = 0;
  const auto* end = key.data() + key.size();
  const auto [ptr, ec] = std::from_chars(key.data(), end, channel);
  if (ec != std::errc{} || ptr != end || key.empty()) {
    return std::nullopt;
  }
  return channel;
}

}  // namespace blf_detail
