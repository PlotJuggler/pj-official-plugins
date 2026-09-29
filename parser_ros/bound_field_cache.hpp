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
/// compare. A miss (a variable-length array changed size, a message with another
/// set of frames) re-resolves that position by name.
///
/// The cache never creates a column: a record with a name it has not learned yet
/// is written by appendRecord, and the handles are picked up only once the host
/// accepted that record. So a record the host rejects (a duplicate field name)
/// creates no column, and fixes no column type, beyond what appendRecord does.
class BoundFieldCache {
 public:
  /// Appends one record: bound when every field is already known, by name otherwise.
  template <typename WriteHost>
  [[nodiscard]] PJ::Status append(
      const WriteHost& host, PJ::Timestamp timestamp, PJ::Span<const PJ::sdk::NamedFieldValue> fields) {
    if (bind(fields, bound_)) {
      auto status =
          host.appendBoundRecord(timestamp, PJ::Span<const PJ::sdk::BoundFieldValue>(bound_.data(), bound_.size()));
      if (status) {
        return status;
      }
      // The host checks a bound record before writing any of it, so a rejected one
      // left nothing behind. Resubmit it by name: the host then reports what it
      // reports by name ("duplicate field name 'x'", not "duplicate field id 3").
    }
    auto status = host.appendRecord(timestamp, fields);
    if (status) {
      learn(host, fields);
    }
    return status;
  }

 private:
  /// Fills `out` with `fields` bound to their handles. Returns false when the
  /// record must be appended by name instead, so the host handles (or rejects,
  /// with its usual message) exactly what it did before: a field the cache has
  /// not learned yet, or a value whose type differs from its column's type.
  [[nodiscard]] bool bind(PJ::Span<const PJ::sdk::NamedFieldValue> fields, std::vector<PJ::sdk::BoundFieldValue>& out) {
    out.clear();
    if (slots_.size() < fields.size()) {
      slots_.resize(fields.size());
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const auto& field = fields[i];
      Slot& slot = slots_[i];
      if (!slot.valid || slot.name != field.name) {
        const auto it = columns_.find(field.name);
        if (it == columns_.end()) {
          return false;
        }
        slot = Slot{.valid = true, .name = field.name, .column = it->second};
      }
      if (!PJ::sdk::isNull(field.value) && PJ::sdk::typeOf(field.value) != slot.column.type) {
        return false;
      }
      out.push_back({.field = slot.column.handle, .value = field.value});
    }
    return true;
  }

  /// Picks up the handle of every field of a record the host just accepted. Its
  /// columns exist by now, so ensureField only fetches them. Nulls, typed or not,
  /// are skipped: their field is learned from a later non-null value. A column
  /// that exists with another type is refused; that field then keeps going
  /// through appendRecord, which is what decides what gets stored.
  template <typename WriteHost>
  void learn(const WriteHost& host, PJ::Span<const PJ::sdk::NamedFieldValue> fields) {
    for (const auto& field : fields) {
      if (PJ::sdk::isNull(field.value) || columns_.contains(field.name)) {
        continue;
      }
      const auto type = PJ::sdk::typeOf(field.value);
      if (auto handle = host.ensureField(field.name, type)) {
        columns_.emplace(field.name, Column{.handle = *handle, .type = type});
      }
    }
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
