#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <pj_utf8_path/utf8_path.hpp>
#include <string>
#include <string_view>
#include <vector>

// Precondition shared by every export_core consumer: `t` is non-decreasing
// (lower_bound, the front/back range pre-filter, and the greedy merge all
// assume it). Producers must sort before handing a series over; duplicate
// timestamps are allowed and keep producer order.
struct NumericSeriesData {
  std::string name;
  std::vector<double> t;
  std::vector<double> v;
};

struct StringSeriesData {
  std::string name;
  std::vector<double> t;
  std::vector<std::string> v;
};

// Generic time-aligned representation shared by both serializers.
struct ExportTable {
  // Numeric columns
  std::vector<std::string> names;
  std::vector<double> time;
  std::vector<std::vector<double>> cols;
  std::vector<std::vector<uint8_t>> has_value;

  // String columns (parallel structure, same time axis)
  std::vector<std::string> string_names;
  std::vector<std::vector<std::string>> string_cols;
  std::vector<std::vector<uint8_t>> string_has_value;
};

using PJ::utf8_path::pathFromUtf8;
using PJ::utf8_path::pathToUtf8;
int indexFromTime(const std::vector<double>& t, double x);
double estimateMinDt(const std::vector<double>& t, size_t start_idx, double t_end);
ExportTable buildExportTable(
    const std::vector<NumericSeriesData>& numeric, const std::vector<StringSeriesData>& strings, double t_start,
    double t_end);
bool serializeCSV(const ExportTable& table, const std::string& path);
bool serializeParquet(const ExportTable& table, const std::string& path);
