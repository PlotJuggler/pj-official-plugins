// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"

namespace ros_parser_detail {

/// Resolves a parser's per-message record to field handles, so the record can
/// be written with appendBoundRecord. appendRecord makes the host normalize,
/// hash and look up every field name of every record; a bound record skips all
/// of that.
///
/// Parsers flatten each message into the same field order, so the cache keeps
/// the name, type and handle last seen at each position: a hit costs one string
/// compare. A miss (a variable-length array changed size, a new message shape)
/// re-resolves that position by name, registering the field on first sight.
class BoundFieldCache {
 public:
  /// Appends one record: bound when every field resolves, by name otherwise.
  template <typename WriteHost>
  [[nodiscard]] PJ::Status append(
      const WriteHost& host, PJ::Timestamp timestamp, PJ::Span<const PJ::sdk::NamedFieldValue> fields) {
    if (bind(host, fields, bound_)) {
      return host.appendBoundRecord(timestamp, PJ::Span<const PJ::sdk::BoundFieldValue>(bound_.data(), bound_.size()));
    }
    return host.appendRecord(timestamp, fields);
  }

 private:
  /// Fills `out` with `fields` bound to their handles. Returns false when the
  /// record must be appended by name instead, so the host handles (or rejects,
  /// with its usual message) exactly what it did before: a field's value type
  /// differs from its column's type, a never-seen field arrives null (no type to
  /// create its column with), or ensureField refused the field.
  template <typename WriteHost>
  [[nodiscard]] bool bind(
      const WriteHost& host, PJ::Span<const PJ::sdk::NamedFieldValue> fields,
      std::vector<PJ::sdk::BoundFieldValue>& out) {
    out.clear();
    if (slots_.size() < fields.size()) {
      slots_.resize(fields.size());
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const auto& field = fields[i];
      const bool is_null = PJ::sdk::isNull(field.value);
      Slot& slot = slots_[i];
      if (!slot.valid || slot.name != field.name) {
        auto it = columns_.find(field.name);
        if (it == columns_.end()) {
          if (is_null) {
            return false;
          }
          const auto type = PJ::sdk::typeOf(field.value);
          auto handle = host.ensureField(field.name, type);
          if (!handle) {
            return false;
          }
          it = columns_.emplace(field.name, Column{.handle = *handle, .type = type}).first;
        }
        slot = Slot{.valid = true, .name = field.name, .column = it->second};
      }
      if (!is_null && PJ::sdk::typeOf(field.value) != slot.column.type) {
        return false;
      }
      out.push_back({.field = slot.column.handle, .value = field.value});
    }
    return true;
  }

  struct Column {
    PJ::sdk::FieldHandle handle;
    PJ::PrimitiveType type;
  };
  struct Slot {
    bool valid = false;
    std::string name;
    Column column{};
  };
  std::vector<Slot> slots_;                          ///< by position in the record
  std::unordered_map<std::string, Column> columns_;  ///< by name, for position misses
  std::vector<PJ::sdk::BoundFieldValue> bound_;      ///< scratch row reused across records
};

}  // namespace ros_parser_detail
