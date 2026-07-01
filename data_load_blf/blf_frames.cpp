#include "blf_frames.hpp"

#include <blf_reader.hh>
#include <blf_structs.hh>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>

namespace blf_detail {

namespace {

/// DBC/BLF extended-frame flag: bit 31 of the CAN id.
constexpr std::uint32_t kExtendedFlag = 0x8000'0000u;
constexpr std::uint32_t kCanIdMask = 0x1FFF'FFFFu;
/// ObjectFlags_e::TimeTenMics — timestamp is in 10-microsecond ticks (else ns).
constexpr std::uint32_t kTimeTenMics = 0x1u;

/// Windows SYSTEMTIME -> nanoseconds since the Unix epoch (treated as UTC).
std::int64_t sysTimeToNs(const lblf::blf_struct::sysTime_t& st) {
  if (st.year == 0) {
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

/// Relative object timestamp in ns, honoring the TimeTenMics/TimeOneNans flag.
std::int64_t objectTimeNs(const lblf::blf_struct::ObjectHeader& obh) {
  const auto flags = static_cast<std::uint32_t>(obh.objectFlags);
  const auto ticks = static_cast<std::int64_t>(obh.objectTimeStamp);
  return (flags & kTimeTenMics) != 0 ? ticks * 10000 : ticks;
}

template <typename CanMsg>
CanFrame toFrame(const CanMsg& can, std::int64_t start_ns) {
  CanFrame frame;
  frame.ts_ns = start_ns + objectTimeNs(can.obh);
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
    const std::int64_t start_ns = sysTimeToNs(reader.getfileStatistics().meas_start_time);
    while (reader.next()) {
      const auto data = reader.data();
      switch (data.base_header.objectType) {
        case lblf::ObjectType_e::CAN_MESSAGE: {
          lblf::blf_struct::CanMessage_obh can{};
          lblf::read_blf_struct(data, can);
          cb(toFrame(can, start_ns));
          ++stats.can_frames;
          break;
        }
        case lblf::ObjectType_e::CAN_MESSAGE2: {
          lblf::blf_struct::CanMessage2_obh can{};
          lblf::read_blf_struct(data, can);
          cb(toFrame(can, start_ns));
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

}  // namespace blf_detail
