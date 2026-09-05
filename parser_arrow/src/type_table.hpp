#pragma once

#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cstdint>
#include <optional>
#include <pj_plugins/sdk/timestamp_policy.hpp>

namespace pj::parser_arrow {

using PJ::sdk::TimestampStorage;

/// Scalar reconstruction selected from a logical type's nanoarrow storage representation.
enum class CopyKind : uint8_t { kUnsupported, kNull, kInt, kUInt, kDouble, kBytes, kInterval, kDecimal };

/// Per-value conversion resolved while planning an output column.
enum class ValueCast : uint8_t {
  kNone,
  kNormalizeBytes,
  kScaleTimestampTicks,
  kWidenToInt64,
  kFloatSecondsToNanoseconds,
};

/// Location or generator from which an output column obtains its values.
enum class ColumnSource : uint8_t { kLeaf, kListElement, kSynthesizedTimestamp };

/// Type-level facts shared by schema planning and scalar reconstruction.
struct TypeRow {
  ArrowType type;
  CopyKind copy;
  bool host_ingestible;
  std::optional<ValueCast> axis_cast;
  TimestampStorage timestamp_storage;
  bool is_unsigned_integer;
  bool is_list_type;
};

/// Sparse rows omit types for which every classification has its unsupported default.
// clang-format off
inline constexpr std::array kTypeTable = std::to_array<TypeRow>({
    // type                                  , copy                  , host_ingestible, axis_cast                            , timestamp_storage, is_unsigned_integer, is_list_type
    // --- null and boolean ---
    {NANOARROW_TYPE_NA                    , CopyKind::kNull       , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_BOOL                  , CopyKind::kInt        , true           , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    // --- signed integers ---
    {NANOARROW_TYPE_INT8                  , CopyKind::kInt        , true           , ValueCast::kWidenToInt64             , TimestampStorage::kNarrowInt       , false              , false},
    {NANOARROW_TYPE_INT16                 , CopyKind::kInt        , true           , ValueCast::kWidenToInt64             , TimestampStorage::kNarrowInt       , false              , false},
    {NANOARROW_TYPE_INT32                 , CopyKind::kInt        , true           , ValueCast::kWidenToInt64             , TimestampStorage::kInt32           , false              , false},
    {NANOARROW_TYPE_INT64                 , CopyKind::kInt        , true           , ValueCast::kWidenToInt64             , TimestampStorage::kInt64           , false              , false},
    // --- unsigned integers ---
    {NANOARROW_TYPE_UINT8                 , CopyKind::kUInt       , true           , ValueCast::kWidenToInt64             , TimestampStorage::kNarrowInt       , true               , false},
    {NANOARROW_TYPE_UINT16                , CopyKind::kUInt       , true           , ValueCast::kWidenToInt64             , TimestampStorage::kNarrowInt       , true               , false},
    {NANOARROW_TYPE_UINT32                , CopyKind::kUInt       , true           , ValueCast::kWidenToInt64             , TimestampStorage::kUInt32          , true               , false},
    {NANOARROW_TYPE_UINT64                , CopyKind::kUInt       , true           , ValueCast::kWidenToInt64             , TimestampStorage::kUInt64          , true               , false},
    // --- floating point ---
    {NANOARROW_TYPE_HALF_FLOAT            , CopyKind::kDouble     , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_FLOAT                 , CopyKind::kDouble     , true           , ValueCast::kFloatSecondsToNanoseconds, TimestampStorage::kFloat32         , false              , false},
    {NANOARROW_TYPE_DOUBLE                , CopyKind::kDouble     , true           , ValueCast::kFloatSecondsToNanoseconds, TimestampStorage::kFloat64         , false              , false},
    // --- strings and binary (large/view variants are normalized to utf8/binary before the host sees them) ---
    {NANOARROW_TYPE_STRING                , CopyKind::kBytes      , true           , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    // host_ingestible is advisory on this row and the view/large rows below it: hostFormat normalizes them to "u"/"z"
    // and answers from the normalized format, never reading the flag. False records why the un-normalized form would
    // be unsafe — PJ4/pj_datastore/src/arrow_import.cpp:210-214 reads int32 offsets after recognizing LARGE_STRING.
    {NANOARROW_TYPE_LARGE_STRING          , CopyKind::kBytes      , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_STRING_VIEW           , CopyKind::kBytes      , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_BINARY                , CopyKind::kBytes      , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_LARGE_BINARY          , CopyKind::kBytes      , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_BINARY_VIEW           , CopyKind::kBytes      , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_FIXED_SIZE_BINARY     , CopyKind::kBytes      , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    // --- temporal (storage INT32/INT64; only TIMESTAMP can be an axis) ---
    {NANOARROW_TYPE_DATE32                , CopyKind::kInt        , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_DATE64                , CopyKind::kInt        , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_TIME32                , CopyKind::kInt        , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_TIME64                , CopyKind::kInt        , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    // kInt is unreachable in production: planning assigns kScaleTimestampTicks to every TIMESTAMP column, so the
    // `cast == kNone` scalar-copy path never sees one. It is declared so supportsScalarCopy() stays honest per type.
    {NANOARROW_TYPE_TIMESTAMP             , CopyKind::kInt        , false          , ValueCast::kScaleTimestampTicks      , TimestampStorage::kNativeTimestamp , false              , false},
    {NANOARROW_TYPE_DURATION              , CopyKind::kInt        , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    // --- intervals and decimals (copied through their dedicated append paths) ---
    {NANOARROW_TYPE_INTERVAL_MONTHS       , CopyKind::kInterval   , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_INTERVAL_DAY_TIME     , CopyKind::kInterval   , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO, CopyKind::kInterval   , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_DECIMAL32             , CopyKind::kDecimal    , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_DECIMAL64             , CopyKind::kDecimal    , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_DECIMAL128            , CopyKind::kDecimal    , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    {NANOARROW_TYPE_DECIMAL256            , CopyKind::kDecimal    , false          , std::nullopt                         , TimestampStorage::kOther           , false              , false},
    // --- lists (expanded to name[i] columns; never copied as scalars) ---
    {NANOARROW_TYPE_LIST                  , CopyKind::kUnsupported, false          , std::nullopt                         , TimestampStorage::kOther           , false              , true},
    {NANOARROW_TYPE_LARGE_LIST            , CopyKind::kUnsupported, false          , std::nullopt                         , TimestampStorage::kOther           , false              , true},
    {NANOARROW_TYPE_FIXED_SIZE_LIST       , CopyKind::kUnsupported, false          , std::nullopt                         , TimestampStorage::kOther           , false              , true},
});
// clang-format on

/// Preserve the queried type in the fallback so diagnostics remain meaningful for omitted complex types.
[[nodiscard]] constexpr TypeRow typeRow(ArrowType type) {
  for (const auto& row : kTypeTable) {
    if (row.type == type) {
      return row;
    }
  }
  return TypeRow{type, CopyKind::kUnsupported, false, std::nullopt, TimestampStorage::kOther, false, false};
}

}  // namespace pj::parser_arrow
