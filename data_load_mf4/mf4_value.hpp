#pragma once

// Type mapping between mdflib channel data types and PlotJuggler column types.
//
// v1 policy (see docs/plans/2026-07-01-mf4-plugin-implementation-plan.md):
//   * numeric channels (integer/float, any endianness) import as CC-applied
//     engineering doubles -> PrimitiveType::kFloat64. Reading integer channels
//     as double via mdflib's GetEngValue is deliberate: it keeps a single read
//     path and preserves the conversion (CC) scaling.
//   * text channels import as strings -> PrimitiveType::kString.
//   * everything else (byte arrays, MIME, CANopen date/time, complex) is
//     unsupported in v1 -> PrimitiveType::kUnspecified; the reader skips such
//     channels with a counted warning.
//
// Enum (value->text) channels are integer channels carrying a CC lookup table.
// Decoding them to text is out of v1 scope, so they map as numeric (kFloat64,
// the underlying code), not kString.

#include <mdf/ichannel.h>

#include <pj_base/type_tree.hpp>

namespace mf4_detail {

/// Map an mdflib ChannelDataType to the PlotJuggler column type used to import
/// it. Returns kUnspecified for types not supported in v1.
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

/// True if the channel is imported as a value column (numeric or string).
inline bool isSupportedValueType(mdf::ChannelDataType type) {
  return mf4TypeToPrimitive(type) != PJ::PrimitiveType::kUnspecified;
}

/// True if the channel is a text channel (read via GetChannelValue<string>
/// rather than GetEngValue<double>).
inline bool isStringType(mdf::ChannelDataType type) {
  return mf4TypeToPrimitive(type) == PJ::PrimitiveType::kString;
}

}  // namespace mf4_detail
