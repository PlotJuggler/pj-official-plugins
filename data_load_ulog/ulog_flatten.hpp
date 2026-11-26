#pragma once

// SDK-free helpers that flatten a ulog_cpp MessageFormat into the ordered scalar leaves PlotJuggler ingests, so the
// traversal can be unit-tested without the plugin SDK. ValueRef construction and PrimitiveType mapping stay in
// ulog_source.cpp.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <ulog_cpp/messages.hpp>
#include <vector>

namespace ulog_flatten {

/// Fields never emitted as series: the message timestamp (the time axis) and ULog padding.
inline bool isSkippedField(const std::string& name) {
  return name == "timestamp" || name.starts_with("_padding");
}

/// A `char[N]` field is a fixed-width string (the ULog spec: no terminating NUL), not N numeric samples. A scalar
/// `char` stays a one-byte numeric leaf.
inline bool isStringField(const ulog_cpp::Field& field) {
  return field.type().type == ulog_cpp::Field::BasicType::CHAR && field.arrayLength() > 0;
}

/// For string leaves `size` spans the whole `char[N]` array.
struct FlatLeaf {
  size_t offset;
  ulog_cpp::Field::BasicType type;
  size_t size;
  bool is_string;
};

/// Flattened field names: nested fields are joined with "." and array elements get ".00", ".01", etc.; a `char[N]`
/// array is one name.
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

/// Visits every leaf in the order of collectFlatFieldNames. `base_offset` is the byte offset of `format` within the
/// raw record (0 at the top level); nested structs accumulate it because ulog_cpp's resolveDefinition assigns
/// per-format 0-based field offsets.
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

/// Minimum raw payload size ULog actually logs for a *top-level* subscribed `format`. Per the spec, a padding field
/// that is the LAST field may be omitted from the data, but is still needed when the format is nested. So only a
/// genuinely trailing padding field of a top-level format is optional; padding anywhere else (including the trailing
/// padding of a nested format, which keeps sibling offsets correct) counts toward the minimum. `format.fields()` is in
/// declaration order, which is also on-disk/offset order, so the last entry is the trailing field.
inline size_t loggedSizeBytes(const ulog_cpp::MessageFormat& format) {
  size_t size = static_cast<size_t>(format.sizeBytes());
  const auto& fields = format.fields();
  if (!fields.empty()) {
    const auto& last = *fields.back();
    if (last.name().starts_with("_padding")) {
      // sizeBytes() sums a name-keyed map, which undercounts duplicate fields; the ordered offset is the real boundary.
      size = static_cast<size_t>(last.offsetInMessage());
    }
  }
  return size;
}

/// Byte offset of the record's `uint64_t timestamp` field, located by NAME: the spec requires the field but not that it
/// comes first. Only the top level is searched (a nested timestamp is that struct's data, not the message clock).
/// Returns nullopt when there is no such field or it is not a scalar uint64.
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

/// View of a `char[size]` string leaf at `offset`. Stops at the first NUL (PX4 zero-pads shorter strings) or at `size`
/// when the string fills the array. Mirrors ulog_cpp's strnlen-based decoding so both agree on string values. The
/// caller must have bounds-checked `offset + size` against the record.
inline std::string_view stringLeafView(const uint8_t* data, size_t offset, size_t size) {
  std::string_view full(reinterpret_cast<const char*>(data + offset), size);
  const auto nul = full.find('\0');
  return nul == std::string_view::npos ? full : full.substr(0, nul);
}

}  // namespace ulog_flatten
