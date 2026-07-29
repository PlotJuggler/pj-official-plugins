#include "arrow_series_reader.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kNanosecondsToSeconds = 1e-9;

struct SeriesColumns {
  std::shared_ptr<arrow::StructArray> root;
  std::shared_ptr<arrow::Int64Array> timestamps;
  std::shared_ptr<arrow::Array> values;
};

std::optional<SeriesColumns> importSeriesColumns(struct ArrowSchema* schema, struct ArrowArray* array) {
  if (schema == nullptr || array == nullptr) {
    if (schema != nullptr && schema->release != nullptr) {
      schema->release(schema);
    }
    if (array != nullptr && array->release != nullptr) {
      array->release(array);
    }
    return std::nullopt;
  }

  // PJ4's series ABI is positional: child 0 is int64 nanoseconds and child 1
  // is the typed value. ImportArray consumes both C structs, including on an
  // import error, so neither raw struct may be released or inspected below.
  auto imported = arrow::ImportArray(array, schema);
  if (!imported.ok()) {
    return std::nullopt;
  }

  std::shared_ptr<arrow::Array> root_array = std::move(imported).ValueUnsafe();
  if (root_array == nullptr || root_array->type_id() != arrow::Type::STRUCT) {
    return std::nullopt;
  }

  auto root = std::static_pointer_cast<arrow::StructArray>(std::move(root_array));
  if (root->num_fields() < 2) {
    return std::nullopt;
  }

  std::shared_ptr<arrow::Array> timestamp_array = root->field(0);
  std::shared_ptr<arrow::Array> values = root->field(1);
  if (timestamp_array == nullptr || timestamp_array->type_id() != arrow::Type::INT64 || values == nullptr) {
    return std::nullopt;
  }

  return SeriesColumns{
      .root = std::move(root),
      .timestamps = std::static_pointer_cast<arrow::Int64Array>(std::move(timestamp_array)),
      .values = std::move(values),
  };
}

template <typename ArrayType>
double numericValue(const std::shared_ptr<arrow::Array>& values, int64_t row) {
  return static_cast<double>(std::static_pointer_cast<ArrayType>(values)->Value(row));
}

std::optional<double> numericValueAt(const std::shared_ptr<arrow::Array>& values, int64_t row) {
  switch (values->type_id()) {
    case arrow::Type::DOUBLE:
      return numericValue<arrow::DoubleArray>(values, row);
    case arrow::Type::FLOAT:
      return numericValue<arrow::FloatArray>(values, row);
    case arrow::Type::INT8:
      return numericValue<arrow::Int8Array>(values, row);
    case arrow::Type::INT16:
      return numericValue<arrow::Int16Array>(values, row);
    case arrow::Type::INT32:
      return numericValue<arrow::Int32Array>(values, row);
    case arrow::Type::INT64:
      return numericValue<arrow::Int64Array>(values, row);
    case arrow::Type::UINT8:
      return numericValue<arrow::UInt8Array>(values, row);
    case arrow::Type::UINT16:
      return numericValue<arrow::UInt16Array>(values, row);
    case arrow::Type::UINT32:
      return numericValue<arrow::UInt32Array>(values, row);
    case arrow::Type::UINT64:
      return numericValue<arrow::UInt64Array>(values, row);
    case arrow::Type::BOOL:
      return std::static_pointer_cast<arrow::BooleanArray>(values)->Value(row) ? 1.0 : 0.0;
    default:
      return std::nullopt;
  }
}

bool isSupportedNumericType(arrow::Type::type type_id) {
  switch (type_id) {
    case arrow::Type::DOUBLE:
    case arrow::Type::FLOAT:
    case arrow::Type::INT8:
    case arrow::Type::INT16:
    case arrow::Type::INT32:
    case arrow::Type::INT64:
    case arrow::Type::UINT8:
    case arrow::Type::UINT16:
    case arrow::Type::UINT32:
    case arrow::Type::UINT64:
    case arrow::Type::BOOL:
      return true;
    default:
      return false;
  }
}

bool rowIsPresent(const SeriesColumns& columns, int64_t row) {
  return !columns.root->IsNull(row) && !columns.timestamps->IsNull(row) && !columns.values->IsNull(row);
}

// The host concatenates sealed chunks in commit order; under out-of-order
// streaming ingest chunks may overlap in time, so the decoded series can step
// backwards at chunk boundaries. Restore the non-decreasing order the export
// pipeline requires; stable_sort keeps duplicate-timestamp rows in delivery
// order. The decode loops detect regressions inline, so this only runs on
// genuinely out-of-order input.
template <typename Value>
void sortByTime(std::vector<double>& t, std::vector<Value>& v) {
  std::vector<size_t> order(t.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) { return t[lhs] < t[rhs]; });

  std::vector<double> sorted_t;
  std::vector<Value> sorted_v;
  sorted_t.reserve(order.size());
  sorted_v.reserve(order.size());
  for (const size_t source : order) {
    sorted_t.push_back(t[source]);
    sorted_v.push_back(std::move(v[source]));
  }
  t = std::move(sorted_t);
  v = std::move(sorted_v);
}

}  // namespace

std::optional<NumericSeriesData> decodeNumericSeries(
    struct ArrowSchema* schema, struct ArrowArray* array, std::string name) {
  auto columns = importSeriesColumns(schema, array);
  if (!columns.has_value()) {
    return std::nullopt;
  }

  if (!isSupportedNumericType(columns->values->type_id())) {
    return std::nullopt;
  }

  NumericSeriesData result;
  result.name = std::move(name);
  result.t.reserve(static_cast<size_t>(columns->root->length()));
  result.v.reserve(static_cast<size_t>(columns->root->length()));

  int64_t prev_ts = std::numeric_limits<int64_t>::min();
  bool needs_sort = false;
  for (int64_t row = 0; row < columns->root->length(); ++row) {
    if (!rowIsPresent(*columns, row)) {
      continue;
    }
    auto value = numericValueAt(columns->values, row);
    if (!value.has_value()) {
      return std::nullopt;
    }
    const int64_t ts = columns->timestamps->Value(row);
    needs_sort = needs_sort || ts < prev_ts;
    prev_ts = ts;
    result.t.push_back(static_cast<double>(ts) * kNanosecondsToSeconds);
    result.v.push_back(*value);
  }
  if (needs_sort) {
    sortByTime(result.t, result.v);
  }
  return result;
}

std::optional<StringSeriesData> decodeStringSeries(
    struct ArrowSchema* schema, struct ArrowArray* array, std::string name) {
  auto columns = importSeriesColumns(schema, array);
  if (!columns.has_value() || columns->values->type_id() != arrow::Type::STRING) {
    return std::nullopt;
  }

  auto values = std::static_pointer_cast<arrow::StringArray>(columns->values);
  StringSeriesData result;
  result.name = std::move(name);
  result.t.reserve(static_cast<size_t>(columns->root->length()));
  result.v.reserve(static_cast<size_t>(columns->root->length()));

  int64_t prev_ts = std::numeric_limits<int64_t>::min();
  bool needs_sort = false;
  for (int64_t row = 0; row < columns->root->length(); ++row) {
    if (!rowIsPresent(*columns, row)) {
      continue;
    }
    const auto view = values->GetView(row);
    const int64_t ts = columns->timestamps->Value(row);
    needs_sort = needs_sort || ts < prev_ts;
    prev_ts = ts;
    result.t.push_back(static_cast<double>(ts) * kNanosecondsToSeconds);
    result.v.emplace_back(view.data(), view.size());
  }
  if (needs_sort) {
    sortByTime(result.t, result.v);
  }
  return result;
}

std::optional<NumericSeriesData> readNumericSeries(
    PJ::sdk::ToolboxHostView& host, PJ::sdk::FieldHandle field, std::string name) {
  PJ::sdk::ArrowSchemaHolder schema_holder;
  PJ::sdk::ArrowArrayHolder array_holder;
  auto status = host.readSeriesArrow(field, schema_holder.out(), array_holder.out());
  if (!status) {
    return std::nullopt;
  }

  auto schema = schema_holder.release();
  auto array = array_holder.release();
  return decodeNumericSeries(&schema, &array, std::move(name));
}

std::optional<StringSeriesData> readStringSeries(
    PJ::sdk::ToolboxHostView& host, PJ::sdk::FieldHandle field, std::string name) {
  PJ::sdk::ArrowSchemaHolder schema_holder;
  PJ::sdk::ArrowArrayHolder array_holder;
  auto status = host.readSeriesArrow(field, schema_holder.out(), array_holder.out());
  if (!status) {
    return std::nullopt;
  }

  auto schema = schema_holder.release();
  auto array = array_holder.release();
  return decodeStringSeries(&schema, &array, std::move(name));
}
