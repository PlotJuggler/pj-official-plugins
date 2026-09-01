#pragma once

// SDK-free helpers that flatten a ulog_cpp MessageFormat into the ordered list
// of scalar leaves PlotJuggler ingests. The traversal logic (field skipping,
// array expansion, string detection, and the nested base-offset accumulation)
// lives here so it can be unit-tested directly, without pulling in the plugin
// SDK. The SDK-coupled pieces (ValueRef construction, PrimitiveType mapping)
// stay in ulog_source.cpp.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <ulog_cpp/messages.hpp>
#include <vector>

namespace ulog_flatten {

/// Fields the importer never emits as time series: the leading message timestamp
/// (used as the time axis) and ULog alignment padding.
inline bool isSkippedField(const std::string& name) {
  return name == "timestamp" || name.starts_with("_padding");
}

/// A `char[N]` field is a fixed-width string per the ULog spec ("Strings
/// (char[length]) do not contain the termination NULL character"), not N
/// numeric samples. A scalar `char` stays a one-byte numeric leaf.
inline bool isStringField(const ulog_cpp::Field& field) {
  return field.type().type == ulog_cpp::Field::BasicType::CHAR && field.arrayLength() > 0;
}

/// One flattened leaf of a record: where it sits, what it is, and how wide it is.
/// For string leaves `size` spans the whole `char[N]` array.
struct FlatLeaf {
  size_t offset;
  ulog_cpp::Field::BasicType type;
  size_t size;
  bool is_string;
};

/// Recursively collect flattened field names for a ulog_cpp MessageFormat.
/// Nested fields are separated by "." and array elements get ".00", ".01", etc.
/// A `char[N]` array collapses to a single name (it is one string series).
inline void collectFlatFieldNames(
    const ulog_cpp::MessageFormat& format, const std::string& prefix, std::vector<std::string>& out) {
  for (const auto& field_ptr : format.fields()) {
    const auto& field = *field_ptr;
    const std::string& name = field.name();
    if (isSkippedField(name)) {
      continue;
    }

    std::string new_prefix = prefix.empty() ? name : prefix + "." + name;

    if (isStringField(field)) {
      out.push_back(new_prefix);
      continue;
    }

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

/// Visit every leaf of a MessageFormat in the same order as
/// collectFlatFieldNames, invoking `visit(const FlatLeaf&)`. `base_offset` is
/// the byte offset of `format` within the raw record (0 at the top level);
/// nested structs accumulate their start offset so each leaf's offset is
/// absolute, matching ulog_cpp's own resolution scheme
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

    if (isStringField(field)) {
      visit(FlatLeaf{field_offset, ulog_cpp::Field::BasicType::CHAR, static_cast<size_t>(count), true});
    } else if (field.type().type == ulog_cpp::Field::BasicType::NESTED) {
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
        visit(FlatLeaf{field_offset + i * elem_size, field.type().type, elem_size, false});
      }
    }
  }
}

/// Byte offset of the record's `uint64_t timestamp` field, located by NAME.
/// The ULog spec requires every subscribed format to carry it but does not
/// require it to be the first field, so a fixed offset-0 read is wrong in
/// general. Only the top level is searched: a timestamp inside a nested
/// struct is that struct's data, not the message clock. Returns nullopt when
/// the format has no such field (or it has the wrong type / is an array).
inline std::optional<size_t> findTimestampOffset(const ulog_cpp::MessageFormat& format) {
  for (const auto& field_ptr : format.fields()) {
    const auto& field = *field_ptr;
    if (field.name() == "timestamp" && field.type().type == ulog_cpp::Field::BasicType::UINT64 &&
        field.arrayLength() < 0 && field.offsetInMessage() >= 0) {
      return static_cast<size_t>(field.offsetInMessage());
    }
  }
  return std::nullopt;
}

/// View of a `char[size]` string leaf at `offset`. Stops at the first NUL (PX4
/// zero-pads shorter strings) or at `size` when the string fills the array
/// (the spec guarantees no terminator). Mirrors ulog_cpp's own strnlen-based
/// decoding, so the plugin and the library agree on string values. The caller
/// must have bounds-checked `offset + size` against the record.
inline std::string_view stringLeafView(const uint8_t* data, size_t offset, size_t size) {
  std::string_view full(reinterpret_cast<const char*>(data + offset), size);
  const auto nul = full.find('\0');
  return nul == std::string_view::npos ? full : full.substr(0, nul);
}

}  // namespace ulog_flatten
