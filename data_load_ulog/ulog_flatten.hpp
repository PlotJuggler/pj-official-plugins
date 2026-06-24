#pragma once

// SDK-free helpers that flatten a ulog_cpp MessageFormat into the ordered list
// of scalar leaves PlotJuggler ingests. The traversal logic (field skipping,
// array expansion, and the nested base-offset accumulation) lives here so it can
// be unit-tested directly, without pulling in the plugin SDK. The SDK-coupled
// pieces (ValueRef construction, PrimitiveType mapping) stay in ulog_source.cpp.

#include <cstddef>
#include <cstdio>
#include <string>
#include <ulog_cpp/messages.hpp>
#include <vector>

namespace ulog_flatten {

/// Fields the importer never emits as time series: the leading message timestamp
/// (used as the time axis) and ULog alignment padding.
inline bool isSkippedField(const std::string& name) {
  return name == "timestamp" || name.starts_with("_padding");
}

/// Recursively collect flattened field names for a ulog_cpp MessageFormat.
/// Nested fields are separated by "." and array elements get ".00", ".01", etc.
inline void collectFlatFieldNames(
    const ulog_cpp::MessageFormat& format, const std::string& prefix, std::vector<std::string>& out) {
  for (const auto& field_ptr : format.fields()) {
    const auto& field = *field_ptr;
    const std::string& name = field.name();
    if (isSkippedField(name)) {
      continue;
    }

    std::string new_prefix = prefix.empty() ? name : prefix + "." + name;

    int arr_len = field.arrayLength();
    int count = (arr_len < 0) ? 1 : arr_len;
    bool is_array = (arr_len > 0);

    for (int i = 0; i < count; ++i) {
      std::string elem_name = new_prefix;
      if (is_array) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), ".%02d", i);
        elem_name += buf;
      }

      if (field.type().type == ulog_cpp::Field::BasicType::NESTED) {
        auto nested_fmt = field.nestedFormat();
        if (nested_fmt) {
          collectFlatFieldNames(*nested_fmt, elem_name, out);
        }
      } else {
        out.push_back(elem_name);
      }
    }
  }
}

/// Visit every scalar leaf of a MessageFormat in the same order as
/// collectFlatFieldNames, invoking `visit(absolute_byte_offset, basic_type,
/// element_size)`. `base_offset` is the byte offset of `format` within the raw
/// record (0 at the top level); nested structs accumulate their start offset so
/// each leaf's offset is absolute, matching ulog_cpp's own resolution scheme
/// (MessageFormat::resolveDefinition assigns per-format 0-based field offsets).
template <typename Visitor>
void forEachFlatLeaf(const ulog_cpp::MessageFormat& format, size_t base_offset, Visitor&& visit) {
  for (const auto& field_ptr : format.fields()) {
    const auto& field = *field_ptr;
    if (isSkippedField(field.name())) {
      continue;
    }

    int arr_len = field.arrayLength();
    int count = (arr_len < 0) ? 1 : arr_len;
    size_t field_offset = base_offset + static_cast<size_t>(field.offsetInMessage());

    if (field.type().type == ulog_cpp::Field::BasicType::NESTED) {
      auto nested_fmt = field.nestedFormat();
      if (!nested_fmt) {
        continue;
      }
      auto nested_size = static_cast<size_t>(nested_fmt->sizeBytes());
      for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        forEachFlatLeaf(*nested_fmt, field_offset + i * nested_size, visit);
      }
    } else {
      size_t elem_size = static_cast<size_t>(field.type().size);
      for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        visit(field_offset + i * elem_size, field.type().type, elem_size);
      }
    }
  }
}

}  // namespace ulog_flatten
