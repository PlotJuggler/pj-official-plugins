#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

// PJ3 toolbox_csv.h:59-72 — generic time-aligned representation shared by both serializers.
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

int indexFromTime(const std::vector<double>& t, double x);
double estimateMinDt(const std::vector<double>& t, size_t start_idx, double t_end);
ExportTable buildExportTable(
    const std::vector<NumericSeriesData>& numeric, const std::vector<StringSeriesData>& strings, double t_start,
    double t_end);
bool serializeCSV(const ExportTable& table, const std::string& path);
bool serializeParquet(const ExportTable& table, const std::string& path);
