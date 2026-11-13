// Merged log of every wired topic, ordered by sample time. Qt-free.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <pj_base/builtin/log.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace log_viewer {

using Level = PJ::sdk::Log::Level;

/// One log entry. Strings that repeat (node, file, function) are interned in the store.
struct LogRow {
  double time = 0.0;      ///< display-axis seconds of the sample
  int64_t stamp_ns = 0;   ///< Log::timestamp_ns, shown as metadata only
  uint64_t seq = 0;       ///< arrival order within the wire
  uint32_t wire = 0;      ///< wire id; orders rows of equal time
  uint32_t node = 0;      ///< interned
  uint32_t file = 0;      ///< interned
  uint32_t function = 0;  ///< interned
  uint32_t line = 0;
  Level level = Level::kUnknown;
  std::string message;
};

/// What a mutation did to the row sequence, so a view can update without starting over.
///
/// In old coordinates: rows [0, dropped_front) were evicted, rows [dropped_front, old_tail) are unchanged,
/// rows [old_tail, old size) were removed. In new coordinates the unchanged rows are
/// [0, old_tail - dropped_front) (clamped at 0) and everything after them is new.
struct StoreChange {
  size_t dropped_front = 0;
  size_t old_tail = 0;
  bool reset = false;  ///< nothing is preserved; ignore the two fields above

  /// A change that touches nothing on a store of @p size rows.
  static StoreChange none(size_t size) {
    return StoreChange{0, size, false};
  }
  /// The change of applying @p next after this one, both expressed against the state before this one.
  void then(const StoreChange& next);
};

class LogStore {
 public:
  static constexpr size_t kDefaultCapacity = 1'000'000;

  explicit LogStore(size_t capacity = kDefaultCapacity) : capacity_(capacity) {
    strings_.emplace_back();  // id 0 = ""
    is_node_.push_back(false);
  }

  /// Id of @p text in the string pool, adding it when new. Id 0 is the empty string.
  uint32_t intern(std::string_view text);
  /// As intern(), and records the string as a node name.
  uint32_t internNode(std::string_view text);
  /// Id of @p text when already pooled.
  [[nodiscard]] std::optional<uint32_t> find(std::string_view text) const;
  [[nodiscard]] const std::string& text(uint32_t id) const {
    return strings_[id];
  }
  /// Node names seen so far, in first-seen order.
  [[nodiscard]] const std::vector<uint32_t>& nodes() const {
    return nodes_;
  }

  /// Add the rows of @p wire. They must be in ascending (time, seq) order and not earlier than the rows
  /// the wire already holds. Evicts the oldest rows beyond the capacity.
  StoreChange insert(uint32_t wire, std::vector<LogRow> rows);
  /// Drop every row of @p wire.
  StoreChange removeWire(uint32_t wire);
  /// Drop the rows of @p wire and insert @p rows in their place.
  StoreChange replaceWire(uint32_t wire, std::vector<LogRow> rows);

  [[nodiscard]] size_t size() const {
    return rows_.size();
  }
  [[nodiscard]] const LogRow& row(size_t index) const {
    return rows_[index];
  }
  /// Whether one of the rows of @p wire has exactly @p time.
  [[nodiscard]] bool wireHasTime(uint32_t wire, double time) const;
  /// Rows of @p wire with time >= @p time.
  [[nodiscard]] size_t wireRowsFrom(uint32_t wire, double time) const;

  /// Identity of a row that survives a reload of its wire: arrival numbers (seq) restart on a reload.
  struct RowKey {
    double time = 0.0;
    uint32_t wire = 0;
    size_t rank = 0;  ///< among the rows of the wire with this time
  };
  [[nodiscard]] RowKey keyOf(size_t index) const;
  /// Index of the row with @p key, if the store holds it.
  [[nodiscard]] std::optional<size_t> indexOf(const RowKey& key) const;

 private:
  void evictOverflow(StoreChange& change);

  size_t capacity_;
  std::deque<LogRow> rows_;
  /// Times of each wire's rows, ascending: the per-wire view of rows_.
  std::map<uint32_t, std::deque<double>> wire_times_;

  std::vector<std::string> strings_;
  /// Hashes std::string and std::string_view alike, so a lookup does not build a std::string.
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view text) const noexcept {
      return std::hash<std::string_view>{}(text);
    }
  };

  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> string_ids_;
  std::vector<bool> is_node_;
  std::vector<uint32_t> nodes_;
};

}  // namespace log_viewer
