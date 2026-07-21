#include "export_core.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <locale.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>

namespace {

// PJ3 toolbox_csv.cpp:112-118 — common serializer preconditions.
bool canSerialize(const ExportTable& table, const std::string& path) {
  return !path.empty() && !table.time.empty() && table.cols.size() == table.names.size() &&
         table.string_cols.size() == table.string_names.size();
}

// PJ3 toolbox_csv.cpp:683,702 — QString::number replacements pinned to the C numeric locale.
std::string formatDouble(double value, const char* format) {
  char buffer[128]{};
  const double normalized_value = (value == 0.0) ? 0.0 : value;
#if defined(_WIN32)
  _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
  if (c_locale != nullptr) {
    (void)_snprintf_l(buffer, sizeof(buffer), format, c_locale, normalized_value);
    _free_locale(c_locale);
  } else {
    (void)std::snprintf(buffer, sizeof(buffer), format, normalized_value);
  }
#else
  locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
  if (c_locale != locale_t{}) {
    locale_t previous_locale = uselocale(c_locale);
    (void)std::snprintf(buffer, sizeof(buffer), format, normalized_value);
    (void)uselocale(previous_locale);
    freelocale(c_locale);
  } else {
    (void)std::snprintf(buffer, sizeof(buffer), format, normalized_value);
  }
#endif
  return buffer;
}

// PJ3 toolbox_csv.cpp:711-727 — quote only string cells that require RFC-4180 escaping.
std::string quoteStringCell(std::string_view value) {
  if (value.find_first_of(",\"\n\r") == std::string_view::npos) {
    return std::string(value);
  }

  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

// PJ3 toolbox_csv.cpp:738-766 — null means missing; present NaN/Inf remain values.
arrow::Result<std::shared_ptr<arrow::Array>> makeDoubleArray(
    const std::vector<double>& values, const std::vector<uint8_t>& present) {
  arrow::DoubleBuilder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<int64_t>(values.size())));

  if (present.size() != values.size()) {
    return arrow::Status::Invalid("size mismatch");
  }

  for (size_t idx = 0; idx < values.size(); ++idx) {
    if (present[idx] == 0) {
      ARROW_RETURN_NOT_OK(builder.AppendNull());
      continue;
    }
    ARROW_RETURN_NOT_OK(builder.Append(values[idx]));
  }

  std::shared_ptr<arrow::Array> array;
  ARROW_RETURN_NOT_OK(builder.Finish(&array));
  return array;
}

// PJ3 toolbox_csv.cpp:768-793 — null means missing; present empty strings remain values.
arrow::Result<std::shared_ptr<arrow::Array>> makeStringArray(
    const std::vector<std::string>& values, const std::vector<uint8_t>& present) {
  arrow::StringBuilder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<int64_t>(values.size())));

  if (present.size() != values.size()) {
    return arrow::Status::Invalid("size mismatch");
  }

  for (size_t idx = 0; idx < values.size(); ++idx) {
    if (present[idx] == 0) {
      ARROW_RETURN_NOT_OK(builder.AppendNull());
      continue;
    }
    ARROW_RETURN_NOT_OK(builder.Append(values[idx]));
  }

  std::shared_ptr<arrow::Array> array;
  ARROW_RETURN_NOT_OK(builder.Finish(&array));
  return array;
}

}  // namespace

std::filesystem::path pathFromUtf8(std::string_view path) {
  std::u8string utf8;
  utf8.reserve(path.size());
  for (const unsigned char byte : path) {
    utf8.push_back(static_cast<char8_t>(byte));
  }
  return std::filesystem::path(std::move(utf8));
}

std::string pathToUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// PJ3 pj3_timeseries.h:151-175 — lower_bound plus strict nearest-neighbor selection.
int indexFromTime(const std::vector<double>& t, double x) {
  if (t.empty()) {
    return -1;
  }

  const auto lower = std::lower_bound(t.begin(), t.end(), x);
  auto index = std::distance(t.begin(), lower);

  if (index >= static_cast<std::ptrdiff_t>(t.size())) {
    return static_cast<int>(t.size() - 1);
  }
  if (index < 0) {
    return 0;
  }

  if (index > 0 && std::abs(t[static_cast<size_t>(index - 1)] - x) < std::abs(t[static_cast<size_t>(index)] - x)) {
    --index;
  }
  return static_cast<int>(index);
}

// PJ3 toolbox_csv.cpp:342-374 — scan at most 2000 intervals for a positive local minimum.
double estimateMinDt(const std::vector<double>& t, size_t start_idx, double t_end) {
  if (t.size() < 2 || start_idx + 1 >= t.size()) {
    return 0.0;
  }

  double min_dt = std::numeric_limits<double>::max();
  const size_t last = std::min(t.size() - 1, start_idx + 2000);

  for (size_t idx = start_idx + 1; idx <= last; ++idx) {
    const double t0 = t[idx - 1];
    const double t1 = t[idx];
    if (t0 > t_end) {
      break;
    }
    const double dt = t1 - t0;
    if (dt > 0.0 && dt < min_dt) {
      min_dt = dt;
    }
  }

  if (min_dt == std::numeric_limits<double>::max()) {
    return 0.0;
  }
  return min_dt;
}

// PJ3 toolbox_csv.cpp:380-647 — adaptive-tolerance greedy merge for numeric and string series.
ExportTable buildExportTable(
    const std::vector<NumericSeriesData>& numeric, const std::vector<StringSeriesData>& strings, double t_start,
    double t_end) {
  ExportTable table;

  struct SeriesRef {
    const NumericSeriesData* series;
    size_t idx;
  };
  struct StringSeriesRef {
    const StringSeriesData* series;
    size_t idx;
  };

  std::vector<SeriesRef> series;
  series.reserve(numeric.size());
  for (const auto& data : numeric) {
    if (data.t.empty() || data.t.front() > t_end || data.t.back() < t_start) {
      continue;
    }
    const int index = indexFromTime(data.t, t_start);
    if (index < 0) {
      continue;
    }
    series.push_back({&data, static_cast<size_t>(index)});
  }

  std::vector<StringSeriesRef> string_series;
  string_series.reserve(strings.size());
  for (const auto& data : strings) {
    if (data.t.empty() || data.t.front() > t_end || data.t.back() < t_start) {
      continue;
    }
    const int index = indexFromTime(data.t, t_start);
    if (index < 0) {
      continue;
    }
    string_series.push_back({&data, static_cast<size_t>(index)});
  }

  if (series.empty() && string_series.empty()) {
    return table;
  }

  double min_dt = 0.0;
  {
    double best = std::numeric_limits<double>::max();
    for (const auto& ref : series) {
      const double dt = estimateMinDt(ref.series->t, ref.idx, t_end);
      if (dt > 0.0 && dt < best) {
        best = dt;
      }
    }
    for (const auto& ref : string_series) {
      const double dt = estimateMinDt(ref.series->t, ref.idx, t_end);
      if (dt > 0.0 && dt < best) {
        best = dt;
      }
    }
    if (best != std::numeric_limits<double>::max()) {
      min_dt = best;
    }
  }

  const double tolerance = (min_dt > 0.0) ? (0.5 * min_dt) : 0.0;
  const size_t numeric_count = series.size();
  const size_t string_count = string_series.size();

  table.names.reserve(numeric_count);
  table.cols.assign(numeric_count, {});
  table.has_value.assign(numeric_count, {});
  for (const auto& ref : series) {
    table.names.push_back(ref.series->name);
  }

  table.string_names.reserve(string_count);
  table.string_cols.assign(string_count, {});
  table.string_has_value.assign(string_count, {});
  for (const auto& ref : string_series) {
    table.string_names.push_back(ref.series->name);
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> row_values(numeric_count, nan);
  std::vector<bool> row_used(numeric_count, false);
  std::vector<std::string> string_row_values(string_count);
  std::vector<bool> string_row_used(string_count, false);

  while (true) {
    bool done = true;
    double min_time = std::numeric_limits<double>::max();

    for (auto& ref : series) {
      if (ref.idx >= ref.series->t.size()) {
        continue;
      }
      const double point_time = ref.series->t[ref.idx];
      if (point_time > t_end) {
        continue;
      }
      done = false;
      if (point_time < min_time) {
        min_time = point_time;
      }
    }
    for (auto& ref : string_series) {
      if (ref.idx >= ref.series->t.size()) {
        continue;
      }
      const double point_time = ref.series->t[ref.idx];
      if (point_time > t_end) {
        continue;
      }
      done = false;
      if (point_time < min_time) {
        min_time = point_time;
      }
    }

    if (done || min_time > t_end) {
      break;
    }

    std::fill(row_values.begin(), row_values.end(), nan);
    std::fill(row_used.begin(), row_used.end(), false);
    std::fill(string_row_used.begin(), string_row_used.end(), false);

    for (size_t col = 0; col < numeric_count; ++col) {
      auto& ref = series[col];
      if (ref.idx >= ref.series->t.size()) {
        continue;
      }
      const double point_time = ref.series->t[ref.idx];
      if (point_time > t_end) {
        continue;
      }
      const bool match = (tolerance == 0.0) ? (point_time == min_time) : (std::abs(point_time - min_time) <= tolerance);
      if (match) {
        row_values[col] = ref.series->v[ref.idx];
        row_used[col] = true;
      }
    }

    for (size_t col = 0; col < string_count; ++col) {
      auto& ref = string_series[col];
      if (ref.idx >= ref.series->t.size()) {
        continue;
      }
      const double point_time = ref.series->t[ref.idx];
      if (point_time > t_end) {
        continue;
      }
      const bool match = (tolerance == 0.0) ? (point_time == min_time) : (std::abs(point_time - min_time) <= tolerance);
      if (match) {
        string_row_values[col] = ref.series->v[ref.idx];
        string_row_used[col] = true;
      }
    }

    table.time.push_back(min_time);
    for (size_t col = 0; col < numeric_count; ++col) {
      table.cols[col].push_back(row_values[col]);
      table.has_value[col].push_back(row_used[col] ? 1 : 0);
      if (row_used[col]) {
        ++series[col].idx;
      }
    }
    for (size_t col = 0; col < string_count; ++col) {
      table.string_cols[col].push_back(string_row_used[col] ? string_row_values[col] : std::string());
      table.string_has_value[col].push_back(string_row_used[col] ? 1 : 0);
      if (string_row_used[col]) {
        ++string_series[col].idx;
      }
    }
  }

  return table;
}

// PJ3 toolbox_csv.cpp:649-735 — text CSV with PJ3 formatting and string-cell quoting.
bool serializeCSV(const ExportTable& table, const std::string& path) {
  if (!canSerialize(table, path)) {
    return false;
  }

  std::ofstream output(pathFromUtf8(path));
  if (!output.is_open()) {
    return false;
  }

  output << "time";
  for (const auto& name : table.names) {
    output << ',' << name;
  }
  for (const auto& name : table.string_names) {
    output << ',' << name;
  }
  output << '\n';

  for (size_t row = 0; row < table.time.size(); ++row) {
    output << formatDouble(table.time[row], "%.6f");

    for (size_t col = 0; col < table.names.size(); ++col) {
      output << ',';
      if (table.has_value[col][row] == 0) {
        continue;
      }
      const double value = table.cols[col][row];
      if (std::isnan(value)) {
        output << "NaN";
      } else if (std::isfinite(value)) {
        output << formatDouble(value, "%.12g");
      } else {
        output << "Inf";
      }
    }

    for (size_t col = 0; col < table.string_names.size(); ++col) {
      output << ',';
      if (table.string_has_value[col][row] != 0) {
        output << quoteStringCell(table.string_cols[col][row]);
      }
    }
    output << '\n';
  }

  return true;
}

// PJ3 toolbox_csv.cpp:737-858 — Arrow table with default Parquet writer properties.
bool serializeParquet(const ExportTable& table, const std::string& path) {
  if (!canSerialize(table, path)) {
    return false;
  }

  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;

  fields.push_back(arrow::field("time", arrow::float64()));
  const std::vector<uint8_t> time_present(table.time.size(), 1);
  auto time_result = makeDoubleArray(table.time, time_present);
  if (!time_result.ok()) {
    return false;
  }
  arrays.push_back(*time_result);

  for (size_t col = 0; col < table.names.size(); ++col) {
    fields.push_back(arrow::field(table.names[col], arrow::float64()));
    auto array_result = makeDoubleArray(table.cols[col], table.has_value[col]);
    if (!array_result.ok()) {
      return false;
    }
    arrays.push_back(*array_result);
  }

  for (size_t col = 0; col < table.string_names.size(); ++col) {
    fields.push_back(arrow::field(table.string_names[col], arrow::utf8()));
    auto array_result = makeStringArray(table.string_cols[col], table.string_has_value[col]);
    if (!array_result.ok()) {
      return false;
    }
    arrays.push_back(*array_result);
  }

  auto schema = std::make_shared<arrow::Schema>(fields);
  auto arrow_table = arrow::Table::Make(schema, arrays);

  // Arrow documents its 8-bit Windows paths as UTF-8 (arrow/util/io_util.h:44-45),
  // and FileOutputStream::Open routes this string through that platform adapter.
  auto output_result = arrow::io::FileOutputStream::Open(path);
  if (!output_result.ok()) {
    return false;
  }
  std::shared_ptr<arrow::io::FileOutputStream> output = *output_result;

  parquet::WriterProperties::Builder builder;
  std::shared_ptr<parquet::WriterProperties> properties = builder.build();
  const auto status =
      parquet::arrow::WriteTable(*arrow_table, arrow::default_memory_pool(), output, 1024 * 64, properties);
  if (!status.ok()) {
    return false;
  }

  return true;
}
