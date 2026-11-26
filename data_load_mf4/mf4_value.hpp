#pragma once

// Type mapping between mdflib channel data types and PlotJuggler column types.
//   * numeric channels (integer/float, any endianness) import as CC-applied engineering doubles
//     (PrimitiveType::kFloat64); GetEngValue on integer channels keeps a single read path and the CC scaling.
//   * text channels import as strings (kString).
//   * everything else (byte arrays, MIME, CANopen date/time, complex) is unsupported (kUnspecified); the reader
//     skips such channels with a counted warning.
// Enum (value->text) channels are integer channels with a CC lookup table; they are not decoded and map as
// numeric (the underlying code).

#include <mdf/ichannel.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <pj_base/type_tree.hpp>

namespace mf4_detail {

/// start_ns + round(t_sec * 1e9), rejecting NaN/infinity, seconds outside the
/// int64 nanosecond range, and additive overflow. Master and CAN timestamps
/// come straight from the file, so they are untrusted input.
inline std::optional<std::int64_t> relativeSecondsToNs(std::int64_t start_ns, double t_sec) {
  // 2^63 ns is about 9.22e9 seconds; 9.0e9 keeps llround comfortably in range.
  if (!(t_sec >= -9.0e9 && t_sec <= 9.0e9)) {  // NaN fails this too
    return std::nullopt;
  }
  const auto rel = static_cast<std::int64_t>(std::llround(t_sec * 1.0e9));
  if ((rel > 0 && start_ns > std::numeric_limits<std::int64_t>::max() - rel) ||
      (rel < 0 && start_ns < std::numeric_limits<std::int64_t>::min() - rel)) {
    return std::nullopt;
  }
  return start_ns + rel;
}

/// kUnspecified for unsupported types.
inline PJ::PrimitiveType mf4TypeToPrimitive(mdf::ChannelDataType type) {
  switch (type) {
    case mdf::ChannelDataType::UnsignedIntegerLe:
    case mdf::ChannelDataType::UnsignedIntegerBe:
    case mdf::ChannelDataType::SignedIntegerLe:
    case mdf::ChannelDataType::SignedIntegerBe:
    case mdf::ChannelDataType::FloatLe:
    case mdf::ChannelDataType::FloatBe:
      return PJ::PrimitiveType::kFloat64;
    case mdf::ChannelDataType::StringAscii:
    case mdf::ChannelDataType::StringUTF8:
    case mdf::ChannelDataType::StringUTF16Le:
    case mdf::ChannelDataType::StringUTF16Be:
      return PJ::PrimitiveType::kString;
    case mdf::ChannelDataType::ByteArray:
    case mdf::ChannelDataType::MimeSample:
    case mdf::ChannelDataType::MimeStream:
    case mdf::ChannelDataType::CanOpenDate:
    case mdf::ChannelDataType::CanOpenTime:
    case mdf::ChannelDataType::ComplexLe:
    case mdf::ChannelDataType::ComplexBe:
      return PJ::PrimitiveType::kUnspecified;
  }
  return PJ::PrimitiveType::kUnspecified;
}

inline bool isSupportedValueType(mdf::ChannelDataType type) {
  return mf4TypeToPrimitive(type) != PJ::PrimitiveType::kUnspecified;
}

/// True if the channel is a text channel (read via GetChannelValue<string>
/// rather than GetEngValue<double>).
inline bool isStringType(mdf::ChannelDataType type) {
  return mf4TypeToPrimitive(type) == PJ::PrimitiveType::kString;
}

}  // namespace mf4_detail
