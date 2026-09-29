// SPDX-License-Identifier: MIT
//
// BoundFieldCache must be indistinguishable from appendRecord as far as the
// datastore is concerned: same columns (names, types, creation order) and the
// same stored rows. The cases that stress it are the dynamic-shape messages
// (tf2_msgs/TFMessage: a vector of structs whose length, order and frame names
// change per message) and messages the host rejects.
//
// SimHost follows the rules of the PJ4 datastore write host that matter here
// (plugin_data_host.cpp): names are normalized in ensureField AND appendRecord;
// appendRecord creates a column lazily from the first non-null value, rejects a
// duplicate name, and skips an untyped null on an unseen field; ensureField
// refuses a type change; appendBoundRecord rejects a duplicate field id.
// A value whose type differs from its column is stored as null (the real host
// converts when exact; both paths share that code, so the model stays simple).

#include "bound_field_cache.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using PJ::PrimitiveType;
using PJ::sdk::BoundFieldValue;
using PJ::sdk::FieldHandle;
using PJ::sdk::NamedFieldValue;
using PJ::sdk::ValueRef;
using ros_parser_detail::BoundFieldCache;

std::string normalize(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (c == '/' && !out.empty() && out.back() == '/') {
      continue;
    }
    out.push_back(c);
  }
  if (!out.empty() && out.front() == '/') {
    out.erase(0, 1);
  }
  return out;
}

std::string show(const ValueRef& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int32_t>) {
          return std::to_string(v);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          return "'" + std::string(v) + "'";
        } else {
          return "?";
        }
      },
      value);
}

class SimHost {
 public:
  [[nodiscard]] PJ::Expected<FieldHandle> ensureField(std::string_view name, PrimitiveType type) const {
    ++ensure_calls;
    const std::string key = normalize(name);
    if (auto it = by_name_.find(key); it != by_name_.end()) {
      if (types_[it->second] != type) {
        return PJ::unexpected("field already exists with a different type");
      }
      return FieldHandle{{1}, it->second};
    }
    return FieldHandle{{1}, create(key, type)};
  }

  [[nodiscard]] PJ::Status appendRecord(PJ::Timestamp ts, PJ::Span<const NamedFieldValue> fields) const {
    std::set<std::string> seen;
    Row row{ts, {}};
    for (const auto& field : fields) {
      const std::string key = normalize(field.name);
      if (!seen.insert(key).second) {
        return PJ::unexpected("duplicate field name '" + key + "'");
      }
      const auto it = by_name_.find(key);
      if (PJ::sdk::isNull(field.value)) {
        if (it != by_name_.end()) {
          row.cells[key] = std::nullopt;
        } else if (const auto* typed = std::get_if<PJ::sdk::TypedNull>(&field.value)) {
          create(key, typed->type);
          row.cells[key] = std::nullopt;
        }
        continue;  // untyped null on an unseen field: nothing to create
      }
      const auto type = PJ::sdk::typeOf(field.value);
      const std::uint32_t id = it != by_name_.end() ? it->second : create(key, type);
      row.cells[key] = types_[id] == type ? std::optional<std::string>(show(field.value)) : std::nullopt;
    }
    rows_.push_back(std::move(row));
    return PJ::okStatus();
  }

  [[nodiscard]] PJ::Status appendBoundRecord(PJ::Timestamp ts, PJ::Span<const BoundFieldValue> fields) const {
    ++bound_appends;
    std::set<std::uint32_t> seen;
    Row row{ts, {}};
    for (const auto& field : fields) {
      if (!seen.insert(field.field.id).second) {
        return PJ::unexpected("duplicate field id " + std::to_string(field.field.id));
      }
      // at(): a handle the host never issued throws, failing the test loudly.
      const bool exact = !PJ::sdk::isNull(field.value) && PJ::sdk::typeOf(field.value) == types_.at(field.field.id);
      row.cells[names_.at(field.field.id)] = exact ? std::optional<std::string>(show(field.value)) : std::nullopt;
    }
    rows_.push_back(std::move(row));
    return PJ::okStatus();
  }

  /// Everything a user can observe: columns in creation order, then every row.
  [[nodiscard]] std::string dump() const {
    std::ostringstream out;
    for (std::size_t i = 0; i < names_.size(); ++i) {
      out << "col " << i << " " << names_[i] << " type=" << static_cast<int>(types_[i]) << "\n";
    }
    for (const auto& row : rows_) {
      out << "row ts=" << row.ts;
      for (const auto& [name, cell] : row.cells) {
        out << " " << name << "=" << (cell ? *cell : "null");
      }
      out << "\n";
    }
    return out.str();
  }

  [[nodiscard]] std::size_t columnCount() const {
    return names_.size();
  }

  mutable int ensure_calls = 0;
  mutable int bound_appends = 0;

 private:
  struct Row {
    PJ::Timestamp ts;
    std::map<std::string, std::optional<std::string>> cells;
  };

  std::uint32_t create(const std::string& key, PrimitiveType type) const {
    const auto id = static_cast<std::uint32_t>(names_.size());
    names_.push_back(key);
    types_.push_back(type);
    by_name_[key] = id;
    return id;
  }

  mutable std::vector<std::string> names_;
  mutable std::vector<PrimitiveType> types_;
  mutable std::map<std::string, std::uint32_t> by_name_;
  mutable std::vector<Row> rows_;
};

using Record = std::vector<NamedFieldValue>;

PJ::Span<const NamedFieldValue> span(const Record& record) {
  return PJ::Span<const NamedFieldValue>(record.data(), record.size());
}

std::string describe(PJ::Timestamp ts, const PJ::Status& status) {
  return "status ts=" + std::to_string(ts) + ": " + (status ? std::string("ok") : status.error()) + "\n";
}

/// Feeds the same records to a plain appendRecord host and to a host behind the
/// cache, and keeps what each append returned, so the test can compare what each
/// one stored and reported.
struct Pair {
  SimHost reference;
  SimHost bound;
  BoundFieldCache cache;
  std::string reference_log;
  std::string bound_log;

  void feed(PJ::Timestamp ts, const Record& record) {
    reference_log += describe(ts, reference.appendRecord(ts, span(record)));
    bound_log += describe(ts, cache.append(bound, ts, span(record)));
  }
};

/// Passes when both hosts hold identical columns and rows and every append
/// returned the same status and error text; otherwise names the first line where
/// they diverge (a full dump of 300 rows is unreadable).
::testing::AssertionResult matchesReference(const Pair& p) {
  const std::string bound = p.bound.dump() + p.bound_log;
  const std::string reference = p.reference.dump() + p.reference_log;
  if (bound == reference) {
    return ::testing::AssertionSuccess();
  }
  std::istringstream in_bound(bound);
  std::istringstream in_reference(reference);
  std::string line_bound;
  std::string line_reference;
  for (int line = 1;; ++line) {
    const bool has_bound = static_cast<bool>(std::getline(in_bound, line_bound));
    const bool has_reference = static_cast<bool>(std::getline(in_reference, line_reference));
    if (has_bound != has_reference || line_bound != line_reference) {
      const auto clip = [](const std::string& text) { return text.size() > 240 ? text.substr(0, 240) + "..." : text; };
      return ::testing::AssertionFailure()
             << "first difference at line " << line << "\n  cache:     " << (has_bound ? clip(line_bound) : "<end>")
             << "\n  reference: " << (has_reference ? clip(line_reference) : "<end>");
    }
  }
}

/// tf2_msgs/TFMessage-shaped record: per transform a fixed set of scalar leaves
/// under "/<parent>/<child>/", but the transforms present, their order and their
/// count differ from message to message. Every leaf of a message carries its own
/// value, so a value written under the wrong name cannot go unnoticed.
Record tfRecord(const std::vector<std::pair<std::string, std::string>>& transforms, double stamp) {
  Record record;
  for (std::size_t t = 0; t < transforms.size(); ++t) {
    const std::string prefix = "/" + transforms[t].first + "/" + transforms[t].second;
    record.push_back({prefix + "/header/stamp", stamp});
    double value = stamp * 1000.0 + static_cast<double>(t) * 10.0;
    for (const char* leaf :
         {"/transform/translation/x", "/transform/translation/y", "/transform/translation/z", "/transform/rotation/x",
          "/transform/rotation/y", "/transform/rotation/z", "/transform/rotation/w"}) {
      value += 1.0;
      record.push_back({prefix + leaf, value});
    }
  }
  return record;
}

const std::vector<std::pair<std::string, std::string>>& tfFrames() {
  static const std::vector<std::pair<std::string, std::string>> frames = {
      {"world", "odom"}, {"odom", "base_link"}, {"base_link", "laser"}, {"laser", "imu"},
      {"imu", "cam"},    {"cam", "arm1"},       {"arm1", "arm2"},       {"odom", "laser"}};
  return frames;
}

}  // namespace

// A message the host rejects must not create more columns than appendRecord does.
// Here the duplicate "/x" comes before "/y": appendRecord creates "/x", stops at
// the duplicate and never creates "/y". A cache that registered "/y" while binding
// would also fix its type (int32, from this rejected message) before any valid
// message arrives.
TEST(BoundFieldCache, RejectedRecordCreatesOnlyWhatAppendRecordCreates) {
  Pair p;
  const Record rejected = {{"/x", 1.0}, {"/x", 2.0}, {"/y", int32_t{5}}};
  const Record valid = {{"/x", 1.0}, {"/y", 2.5}};

  p.feed(1, rejected);
  EXPECT_TRUE(matchesReference(p)) << "a rejected message registered columns";
  EXPECT_EQ(p.bound.columnCount(), 1U);  // only "x", exactly what appendRecord created

  p.feed(2, valid);
  EXPECT_TRUE(matchesReference(p));
  EXPECT_NE(p.bound.dump().find("y=2.5"), std::string::npos) << "y must have been created as double by the valid row";
}

// Dynamic-size vector of structs: which transforms are present, and in which
// order, changes per message, so nearly every field lands on a different
// position than last time. Stored data and column order must not care.
TEST(BoundFieldCache, TfLikeShiftingShapesStoreExactlyWhatAppendRecordStores) {
  for (unsigned seed = 1; seed <= 50; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937 rng(seed);
    Pair p;
    for (int i = 0; i < 300; ++i) {
      auto transforms = tfFrames();
      std::shuffle(transforms.begin(), transforms.end(), rng);
      transforms.resize(1 + rng() % transforms.size());
      p.feed(i, tfRecord(transforms, static_cast<double>(i)));
    }
    ASSERT_TRUE(matchesReference(p));
  }
}

// Same, with TFMessages that repeat a parent/child pair (rejected by the host,
// which is what appendRecord does too), placed anywhere in the message, and
// with a leaf whose type flips (a rejected message must not decide a type).
TEST(BoundFieldCache, TfLikeRejectedMessagesLeaveNoTrace) {
  for (unsigned seed = 1; seed <= 100; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937 rng(seed);
    Pair p;
    std::deque<std::string> strings;
    for (int i = 0; i < 300; ++i) {
      auto transforms = tfFrames();
      std::shuffle(transforms.begin(), transforms.end(), rng);
      transforms.resize(1 + rng() % transforms.size());
      if (rng() % 8 == 0) {
        transforms.insert(
            transforms.begin() + static_cast<std::ptrdiff_t>(rng() % (transforms.size() + 1)),
            transforms[rng() % transforms.size()]);
      }
      Record record = tfRecord(transforms, static_cast<double>(i));
      for (auto& field : record) {
        if (field.name.ends_with("/rotation/w")) {
          switch (rng() % 20) {
            case 0:
              field.value = int32_t{1};
              break;
            case 1:
              strings.emplace_back("x");
              field.value = std::string_view(strings.back());
              break;
            case 2:
              field.value = PJ::NullValue{};
              break;
            case 3:
              field.value = PJ::sdk::TypedNull{PrimitiveType::kFloat64};
              break;
            default:
              break;
          }
        }
      }
      p.feed(i, record);
    }
    ASSERT_TRUE(matchesReference(p));
  }
}

// The point of the cache: once the names are known, records are written by
// handle and the host no longer resolves a single name, however the shape moves.
TEST(BoundFieldCache, SteadyStateWritesByHandleWithoutResolvingNames) {
  std::mt19937 rng(7);
  SimHost host;
  BoundFieldCache cache;
  const auto feed = [&](PJ::Timestamp ts) {
    auto transforms = tfFrames();
    std::shuffle(transforms.begin(), transforms.end(), rng);
    transforms.resize(1 + rng() % transforms.size());
    const Record record = tfRecord(transforms, static_cast<double>(ts));
    ASSERT_TRUE(cache.append(host, ts, span(record)));
  };

  for (int i = 0; i < 200; ++i) {  // enough for every frame pair to have appeared
    feed(i);
  }
  const int ensure_before = host.ensure_calls;
  const int bound_before = host.bound_appends;
  for (int i = 200; i < 400; ++i) {
    feed(i);
  }
  EXPECT_EQ(host.ensure_calls, ensure_before) << "names were re-resolved after warm-up";
  EXPECT_EQ(host.bound_appends - bound_before, 200) << "not every record took the by-handle path";
}

// A known column receiving another type: the record goes by name, so the host
// decides what is stored (converted when exact, otherwise null plus a warning).
TEST(BoundFieldCache, ValueOfAnotherTypeIsLeftToAppendRecord) {
  Pair p;
  p.feed(1, {{"/v", 1.0}, {"/w", 2.0}});  // learns both
  p.feed(2, {{"/v", 1.5}, {"/w", 2.5}});  // by handle
  const int bound_before = p.bound.bound_appends;
  p.feed(3, {{"/v", int32_t{4}}, {"/w", 3.0}});
  EXPECT_EQ(p.bound.bound_appends, bound_before) << "a value of another type was written by handle";
  p.feed(4, {{"/v", 3.0}, {"/w", 3.5}});
  EXPECT_TRUE(matchesReference(p));
}

// A message repeating a known field is rejected with the host's by-name error,
// which names the field, not with the bound path's "duplicate field id N".
TEST(BoundFieldCache, DuplicateOfKnownFieldReportsTheFieldName) {
  SimHost host;
  BoundFieldCache cache;
  const Record first = {{"/x", 1.0}};
  const Record duplicate = {{"/x", 1.0}, {"/x", 2.0}};
  ASSERT_TRUE(cache.append(host, 1, span(first)));
  const auto status = cache.append(host, 2, span(duplicate));
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "duplicate field name 'x'");
}

// "/a//b" and "a/b" are one field for the host. Learned under both spellings they
// must write the same column, and together in one message they are a duplicate.
TEST(BoundFieldCache, SpellingsTheHostMergesStayOneField) {
  Pair p;
  p.feed(1, {{"/a//b", 1.0}});
  p.feed(2, {{"a/b", 2.0}});
  p.feed(3, {{"/a//b", 3.0}});
  p.feed(4, {{"/a//b", 4.0}, {"a/b", 5.0}});
  p.feed(5, {{"a/b", 6.0}});
  EXPECT_TRUE(matchesReference(p));
  EXPECT_EQ(p.bound.columnCount(), 1U);
}

// Null handling on a field never seen before: typed null creates its column,
// untyped null creates nothing; the first real value then decides the type.
TEST(BoundFieldCache, NullsOnUnseenFieldsBehaveLikeAppendRecord) {
  Pair p;
  p.feed(1, {{"/typed", PJ::sdk::TypedNull{PrimitiveType::kFloat64}}, {"/untyped", PJ::NullValue{}}, {"/a", 1.0}});
  p.feed(2, {{"/typed", 2.0}, {"/untyped", 3.0}, {"/a", PJ::NullValue{}}});
  p.feed(3, {{"/typed", 2.0}, {"/untyped", 3.0}, {"/a", 5.0}});
  EXPECT_TRUE(matchesReference(p));
}

// A column that already exists with another type (created by someone else, or
// by an earlier parser instance on the same topic): the explicit ensureField is
// refused, and the cache must keep deferring to appendRecord without failing.
TEST(BoundFieldCache, ColumnOfAnotherTypeIsHandledByAppendRecord) {
  Pair p;
  ASSERT_TRUE(p.reference.ensureField("/z", PrimitiveType::kInt32));
  ASSERT_TRUE(p.bound.ensureField("/z", PrimitiveType::kInt32));
  for (int i = 0; i < 3; ++i) {
    p.feed(i, {{"/z", 1.5}, {"/k", 2.0}});
  }
  EXPECT_TRUE(matchesReference(p));
}
