// Store, loader and filter of the log viewer, without Qt. The loader reads a fake host through the real
// pj.widget_host.v1 bridge and WidgetHostView, as the plugin does.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <string>
#include <vector>

#include "fake_log_host.hpp"
#include "log_filter.hpp"
#include "log_loader.hpp"
#include "log_store.hpp"

namespace {

using log_viewer::CompiledFilter;
using log_viewer::Level;
using log_viewer::LogFilter;
using log_viewer::LogLoader;
using log_viewer::LogRow;
using log_viewer::LogStore;
using log_viewer_test::FakeLogHost;
using log_viewer_test::makeLog;

LogRow row(double time, uint64_t seq, std::string message = {}, Level level = Level::kInfo) {
  LogRow out;
  out.time = time;
  out.seq = seq;
  out.level = level;
  out.message = std::move(message);
  return out;
}

std::vector<double> times(const LogStore& store) {
  std::vector<double> out;
  for (size_t i = 0; i < store.size(); ++i) {
    out.push_back(store.row(i).time);
  }
  return out;
}

/// Rows the store holds for @p wire, by its per-wire index.
size_t wireRows(const LogStore& store, uint32_t wire) {
  return store.wireRowsFrom(wire, std::numeric_limits<double>::lowest());
}

// ---------------------------------------------------------------------------------------------- store

TEST(LogStore, EqualTimesOrderByWireThenArrival) {
  LogStore store;
  store.insert(1, {row(1.0, 0, "w1 a"), row(2.0, 1, "w1 b"), row(2.0, 2, "w1 c")});
  store.insert(0, {row(2.0, 0, "w0 a"), row(3.0, 1, "w0 b")});
  ASSERT_EQ(store.size(), 5u);
  const std::vector<std::string> expected = {"w1 a", "w0 a", "w1 b", "w1 c", "w0 b"};
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(store.row(i).message, expected[i]) << i;
  }
}

TEST(LogStore, RevealCountAtTheEdges) {
  LogStore store;
  std::vector<uint32_t> all;
  EXPECT_EQ(log_viewer::revealedCount(store, all, 10.0), 0u);
  store.insert(0, {row(1.0, 0), row(2.0, 1), row(2.0, 2), row(3.0, 3)});
  all = {0, 1, 2, 3};
  EXPECT_EQ(log_viewer::revealedCount(store, all, 0.5), 0u);
  EXPECT_EQ(log_viewer::revealedCount(store, all, 1.0), 1u);
  EXPECT_EQ(log_viewer::revealedCount(store, all, 1.5), 1u);
  EXPECT_EQ(log_viewer::revealedCount(store, all, 2.0), 3u);
  EXPECT_EQ(log_viewer::revealedCount(store, all, 3.0), 4u);
  EXPECT_EQ(log_viewer::revealedCount(store, all, 1e300), 4u);
  EXPECT_EQ(log_viewer::revealedCount(store, all, -1e300), 0u);
}

TEST(LogStore, RemoveWireKeepsTheOthers) {
  LogStore store;
  store.insert(0, {row(1.0, 0), row(3.0, 1)});
  store.insert(1, {row(2.0, 0), row(4.0, 1)});
  const auto change = store.removeWire(0);
  EXPECT_EQ(change.old_tail, 0u);
  EXPECT_EQ(times(store), (std::vector<double>{2.0, 4.0}));
  EXPECT_EQ(wireRows(store, 0), 0u);
  EXPECT_EQ(wireRows(store, 1), 2u);
}

TEST(LogStore, CapacityDropsTheOldest) {
  LogStore store(5);
  store.insert(0, {row(1.0, 0), row(2.0, 1), row(3.0, 2)});
  const auto change = store.insert(1, {row(4.0, 0), row(5.0, 1), row(6.0, 2), row(7.0, 3)});
  EXPECT_EQ(change.dropped_front, 2u);
  EXPECT_EQ(change.old_tail, 3u);
  EXPECT_EQ(times(store), (std::vector<double>{3.0, 4.0, 5.0, 6.0, 7.0}));
  EXPECT_EQ(wireRows(store, 0), 1u);
  EXPECT_EQ(wireRows(store, 1), 4u);
}

TEST(LogStore, RowKeySurvivesAReloadThatRenumbersTheWire) {
  LogStore store;
  store.insert(0, {row(1.0, 0, "w0 a"), row(2.0, 1, "w0 b"), row(2.0, 2, "w0 c")});
  store.insert(1, {row(2.0, 0, "w1 a")});
  const LogStore::RowKey key = store.keyOf(2);  // "w0 c", the second wire-0 row at 2.0
  ASSERT_EQ(store.row(2).message, "w0 c");
  // A late insert before it: the reload numbers every row anew, so its seq moves from 2 to 3.
  store.replaceWire(0, {row(0.5, 0, "w0 late"), row(1.0, 1, "w0 a"), row(2.0, 2, "w0 b"), row(2.0, 3, "w0 c")});
  const auto found = store.indexOf(key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(store.row(*found).message, "w0 c");
  store.removeWire(0);
  EXPECT_FALSE(store.indexOf(key).has_value());
}

TEST(LogStore, DefaultCapacityIsOneMillion) {
  LogStore store;
  std::vector<LogRow> rows;
  rows.reserve(1'000'010);
  for (uint64_t i = 0; i < 1'000'010; ++i) {
    rows.push_back(row(static_cast<double>(i), i));
  }
  const auto change = store.insert(0, std::move(rows));
  EXPECT_EQ(store.size(), 1'000'000u);
  EXPECT_EQ(change.dropped_front, 10u);
  EXPECT_EQ(store.row(0).time, 10.0);
}

TEST(LogStore, KeptRowsMoveByTheEvictionAndOthersAreNotKept) {
  const log_viewer::StoreChange change{2, 6, false};  // rows 0..1 evicted, 2..5 unchanged, 6.. rebuilt
  EXPECT_FALSE(change.kept(1).has_value());
  EXPECT_EQ(change.kept(2), 0u);
  EXPECT_EQ(change.kept(5), 3u);
  EXPECT_FALSE(change.kept(6).has_value());
  EXPECT_FALSE((log_viewer::StoreChange{0, 6, true}.kept(3).has_value()));
}

TEST(LogStore, ChangesCompose) {
  LogStore store;
  store.insert(0, {row(1.0, 0), row(2.0, 1), row(3.0, 2)});
  auto change = log_viewer::StoreChange::none(store.size());
  change.then(store.insert(1, {row(2.5, 0)}));  // old_tail 2
  change.then(store.insert(0, {row(4.0, 3)}));  // appended: old_tail 4 in the middle state
  EXPECT_FALSE(change.reset);
  EXPECT_EQ(change.dropped_front, 0u);
  EXPECT_EQ(change.old_tail, 2u);
}

// --------------------------------------------------------------------------------------------- loader

class LogLoaderTest : public ::testing::Test {
 protected:
  LogLoaderTest() : view_(host_.makeHost()) {}

  log_viewer::SyncResult sync(double tracker = 1e9) {
    const auto values = host_.values(tracker);
    return loader_.sync(PJ::Span<const PJ_widget_value_t>(values.data(), values.size()), view_);
  }

  void fill(uint32_t slot, const std::vector<double>& at) {
    for (double t : at) {
      host_.add(slot, t, makeLog(Level::kInfo, "t=" + std::to_string(t)));
    }
  }

  FakeLogHost host_;
  PJ::WidgetHostView view_;
  LogStore store_;
  LogLoader loader_{store_};
};

TEST_F(LogLoaderTest, FullLoadAsksForTheWholeCount) {
  host_.wire(0, "/rosout");
  std::vector<double> at;
  for (int i = 0; i < 25'000; ++i) {
    at.push_back(i * 0.01);
  }
  fill(0, at);
  sync();
  EXPECT_EQ(store_.size(), 25'000u);
  EXPECT_EQ(loader_.receivedSamples(), loader_.hostSamples());
  // The count read asks for one sample; the load asks for everything, beyond the default cap.
  ASSERT_GE(host_.queries.size(), 2u);
  EXPECT_EQ(host_.queries[0].max_samples, 1u);
  EXPECT_EQ(host_.queries[1].max_samples, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(store_.row(0).message, "t=0.000000");
}

TEST_F(LogLoaderTest, DecodesEveryField) {
  host_.wire(0, "/rosout");
  host_.add(0, 1.5, makeLog(Level::kWarning, "two\nlines", "/talker", "motion", 1'700'000'000'123'456'789));
  sync();
  ASSERT_EQ(store_.size(), 1u);
  const LogRow& r = store_.row(0);
  EXPECT_EQ(r.time, 1.5);
  EXPECT_EQ(r.level, Level::kWarning);
  EXPECT_EQ(r.message, "two\nlines");
  EXPECT_EQ(store_.text(r.node), "/talker");
  EXPECT_EQ(store_.text(r.category), "motion");
  EXPECT_EQ(store_.categories(), (std::vector<uint32_t>{r.category}));
  EXPECT_EQ(r.stamp_ns, 1'700'000'000'123'456'789);
}

TEST_F(LogLoaderTest, StripsTerminalColours) {
  host_.wire(0, "/rosout");
  host_.add(0, 1.0, makeLog(Level::kInfo, "\x1b[34m\x1b[1mActivating planner_server\x1b[0m\x1b[0m"));
  sync();
  ASSERT_EQ(store_.size(), 1u);
  EXPECT_EQ(store_.row(0).message, "Activating planner_server");
}

TEST(StripTerminalEscapes, KeepsTextAndDropsSequences) {
  const auto strip = [](std::string text) {
    log_viewer::stripTerminalEscapes(text);
    return text;
  };
  EXPECT_EQ(strip("plain text"), "plain text");
  EXPECT_EQ(strip("\x1b[1;31mred\x1b[0m and \x1b[38;5;208morange\x1b[m"), "red and orange");
  EXPECT_EQ(strip("a\x1b]0;title\ab"), "ab");
  EXPECT_EQ(strip("a\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\b"), "alinkb");
  EXPECT_EQ(strip("keep\nlines\x1b[0m"), "keep\nlines");
  EXPECT_EQ(strip("truncated \x1b[31"), "truncated ");
  EXPECT_EQ(strip("trailing \x1b"), "trailing ");
  EXPECT_EQ(strip("\x1b(Bcharset"), "charset");
}

TEST_F(LogLoaderTest, IncrementalAppendsOnlyNewRows) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0, 3.0, 3.0});
  sync();
  ASSERT_EQ(store_.size(), 4u);
  fill(0, {4.0, 5.0});
  const auto result = sync();
  EXPECT_EQ(loader_.fullLoads(), 1u);
  EXPECT_EQ(loader_.incrementalLoads(), 1u);
  EXPECT_EQ(times(store_), (std::vector<double>{1.0, 2.0, 3.0, 3.0, 4.0, 5.0}));
  // A pure append: the rows already shown are untouched.
  EXPECT_FALSE(result.change.reset);
  EXPECT_EQ(result.change.dropped_front, 0u);
  EXPECT_EQ(result.change.old_tail, 4u);
  // The tail read started at the last time held and the repeated samples at 3.0 were not duplicated.
  EXPECT_EQ(host_.queries.back().from, 3.0);

  // Nothing new: one count read, nothing loaded.
  const size_t before = host_.queries.size();
  sync();
  EXPECT_EQ(host_.queries.size(), before + 1);
  EXPECT_EQ(store_.size(), 6u);
}

TEST_F(LogLoaderTest, LateInsertReloads) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0, 3.0});
  sync();
  fill(0, {1.5});
  sync();
  EXPECT_EQ(loader_.fullLoads(), 2u);
  EXPECT_EQ(times(store_), (std::vector<double>{1.0, 1.5, 2.0, 3.0}));

  // At exactly the last time held, too.
  fill(0, {3.0});
  sync();
  EXPECT_EQ(loader_.fullLoads(), 3u);
  EXPECT_EQ(times(store_), (std::vector<double>{1.0, 1.5, 2.0, 3.0, 3.0}));
}

TEST_F(LogLoaderTest, OffsetShiftReloads) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0, 3.0});
  sync();
  host_.display_offset -= std::chrono::seconds(10);  // the display origin moves 10 s earlier
  sync();
  EXPECT_EQ(loader_.fullLoads(), 2u);
  EXPECT_EQ(times(store_), (std::vector<double>{11.0, 12.0, 13.0}));
}

TEST_F(LogLoaderTest, HostEvictionKeepsTheRowsShown) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0, 3.0, 4.0, 5.0});
  sync();
  auto& samples = host_.samples(0);
  samples.erase(samples.begin(), samples.begin() + 2);
  sync();
  EXPECT_EQ(loader_.fullLoads(), 1u);
  EXPECT_EQ(store_.size(), 5u);

  // Evicted and appended in one step.
  samples.erase(samples.begin());
  fill(0, {6.0, 7.0});
  sync();
  EXPECT_EQ(loader_.fullLoads(), 1u);
  EXPECT_EQ(loader_.incrementalLoads(), 1u);
  EXPECT_EQ(times(store_), (std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0}));
}

TEST_F(LogLoaderTest, OlderHostCapKeepsWhatArrivedAndReportsIt) {
  host_.wire(0, "/rosout");
  host_.hard_cap = 100;
  std::vector<double> at;
  for (int i = 0; i < 250; ++i) {
    at.push_back(i);
  }
  fill(0, at);
  sync();
  EXPECT_EQ(store_.size(), 100u);
  EXPECT_EQ(loader_.receivedSamples(), 100u);
  EXPECT_EQ(loader_.hostSamples(), 250u);
  // Unchanged history: no new full load on the next update.
  sync();
  EXPECT_EQ(loader_.fullLoads(), 1u);
}

TEST_F(LogLoaderTest, NonLogWireWarnsOnceAndShowsNothing) {
  auto& image = host_.wire(0, "/camera");
  image.type_name = "kImage";
  host_.addBytes(0, 1.0, {1, 2, 3});
  auto first = sync();
  ASSERT_EQ(first.warnings.size(), 1u);
  EXPECT_EQ(first.warnings[0], "/camera is not a log topic");
  EXPECT_EQ(store_.size(), 0u);
  EXPECT_TRUE(sync().warnings.empty());
  ASSERT_EQ(loader_.wires().size(), 1u);
  EXPECT_FALSE(loader_.wires()[0].is_log);
}

// The host counts an entry it cannot deliver (a message its parser rejects, a failed fetch): the wire is
// still a log topic, and an unchanged history must not be reloaded or dropped.
TEST_F(LogLoaderTest, UndeliverableFirstSampleKeepsTheWire) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0, 3.0});
  host_.samples(0).front().undeliverable = true;
  const auto first = sync();
  EXPECT_TRUE(first.warnings.empty());
  ASSERT_EQ(loader_.wires().size(), 1u);
  EXPECT_TRUE(loader_.wires()[0].is_log);
  EXPECT_EQ(times(store_), (std::vector<double>{2.0, 3.0}));

  sync();
  EXPECT_EQ(times(store_), (std::vector<double>{2.0, 3.0}));
  EXPECT_EQ(loader_.fullLoads(), 1u);

  fill(0, {4.0});
  sync();
  EXPECT_EQ(times(store_), (std::vector<double>{2.0, 3.0, 4.0}));
  EXPECT_TRUE(loader_.wires()[0].is_log);
}

// The first sample tells an offset change from an unchanged history; without it, the last one does.
TEST_F(LogLoaderTest, UndeliverableFirstSampleStillSeesAnOffsetShift) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0, 3.0});
  host_.samples(0).front().undeliverable = true;
  sync();
  host_.display_offset -= std::chrono::seconds(10);
  sync();
  EXPECT_EQ(times(store_), (std::vector<double>{12.0, 13.0}));
}

TEST_F(LogLoaderTest, UnresolvedWireDoesNotWarnWhileItsValueIsNone) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0});
  host_.fail_with = "not resolved yet";
  // Tracker before the first sample: the wire's value is NONE.
  ASSERT_EQ(host_.values(0.5)[0].kind, PJ_WIDGET_VALUE_NONE);
  EXPECT_TRUE(sync(0.5).warnings.empty());
  EXPECT_TRUE(sync(0.5).warnings.empty());
  EXPECT_EQ(store_.size(), 0u);
  // The wire resolves: the next update loads it.
  host_.fail_with.clear();
  sync();
  EXPECT_EQ(store_.size(), 2u);
  EXPECT_TRUE(host_.warnings().empty());
}

TEST_F(LogLoaderTest, FailingHistoryWithAValueWarnsOnce) {
  host_.wire(0, "/rosout");
  fill(0, {1.0, 2.0});
  host_.fail_with = "not resolved yet";
  ASSERT_NE(host_.values(1e9)[0].kind, PJ_WIDGET_VALUE_NONE);
  const auto first = sync();
  ASSERT_EQ(first.warnings.size(), 1u);
  EXPECT_NE(first.warnings[0].find("cannot read its history"), std::string::npos);
  EXPECT_TRUE(sync().warnings.empty());
}

TEST_F(LogLoaderTest, VanishedWireDropsItsRows) {
  host_.wire(0, "/rosout");
  host_.wire(1, "/other/rosout");
  fill(0, {1.0, 3.0});
  fill(1, {2.0});
  sync();
  ASSERT_EQ(store_.size(), 3u);
  host_.wires.erase({FakeLogHost::kInput, 1});
  const auto result = sync();
  EXPECT_TRUE(result.wires_changed);
  EXPECT_EQ(times(store_), (std::vector<double>{1.0, 3.0}));
}

TEST_F(LogLoaderTest, TwoWiresMergeByTime) {
  host_.wire(0, "/a");
  host_.wire(1, "/b");
  fill(0, {1.0, 3.0});
  fill(1, {2.0, 3.0});
  sync();
  EXPECT_EQ(times(store_), (std::vector<double>{1.0, 2.0, 3.0, 3.0}));
  EXPECT_EQ(store_.row(2).wire, 0u);  // equal time: wire order
  EXPECT_EQ(store_.row(3).wire, 1u);
}

// --------------------------------------------------------------------------------------------- filter

class LogFilterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    add(0, 1.0, Level::kDebug, "/a", "Starting motor");
    add(0, 2.0, Level::kInfo, "/a", "motor ready");
    add(1, 3.0, Level::kWarning, "/b", "Battery LOW");
    add(1, 4.0, Level::kError, "/b", "motor fault");
    add(0, 5.0, Level::kFatal, "/a", "abort");
    add(1, 6.0, Level::kUnknown, "/b", "no level");
  }

  void add(
      uint32_t wire, double time, Level level, const std::string& node, const std::string& message,
      const std::string& category = {}) {
    LogRow r = row(time, seq_++, message, level);
    r.node = store_.internNode(node);
    r.category = store_.internCategory(category);
    store_.insert(wire, {r});
  }

  std::vector<double> pass(const LogFilter& filter) const {
    std::vector<uint32_t> indices;
    CompiledFilter(filter, store_).collect(store_, 0, store_.size(), indices);
    std::vector<double> out;
    for (uint32_t index : indices) {
      out.push_back(store_.row(index).time);
    }
    return out;
  }

  LogStore store_;
  uint64_t seq_ = 0;
};

TEST_F(LogFilterTest, LevelsNodeTextTopic) {
  LogFilter all;
  EXPECT_EQ(pass(all).size(), 6u);

  LogFilter levels;
  levels.levels[static_cast<size_t>(Level::kDebug)] = false;
  levels.levels[static_cast<size_t>(Level::kInfo)] = false;
  EXPECT_EQ(pass(levels), (std::vector<double>{3.0, 4.0, 5.0, 6.0}));

  LogFilter node;
  node.node = "/b";
  EXPECT_EQ(pass(node), (std::vector<double>{3.0, 4.0, 6.0}));
  node.node = "/never-seen";
  EXPECT_TRUE(pass(node).empty());

  LogFilter text;
  text.text = "MOTOR";
  EXPECT_EQ(pass(text), (std::vector<double>{1.0, 2.0, 4.0}));
  text.text = "low";
  EXPECT_EQ(pass(text), (std::vector<double>{3.0}));

  LogFilter topic;
  topic.hidden_wires = {1};
  EXPECT_EQ(pass(topic), (std::vector<double>{1.0, 2.0, 5.0}));

  LogFilter combined;
  combined.levels[static_cast<size_t>(Level::kError)] = false;
  combined.node = "/a";
  combined.text = "motor";
  EXPECT_EQ(pass(combined), (std::vector<double>{1.0, 2.0}));
  combined.hidden_wires = {0};
  EXPECT_TRUE(pass(combined).empty());

  LogFilter error_on_b;
  error_on_b.node = "/b";
  error_on_b.text = "motor";
  EXPECT_EQ(pass(error_on_b), (std::vector<double>{4.0}));
}

TEST_F(LogFilterTest, Category) {
  add(0, 7.0, Level::kInfo, "/a", "joint limit", "motion");
  add(1, 8.0, Level::kInfo, "/b", "link up", "net");
  EXPECT_EQ(store_.categories().size(), 2u);  // the empty category of the others is not one

  LogFilter category;
  category.category = "motion";
  EXPECT_EQ(pass(category), (std::vector<double>{7.0}));
  category.node = "/b";
  EXPECT_TRUE(pass(category).empty());
  category = {};
  category.category = "never-seen";
  EXPECT_TRUE(pass(category).empty());
}

TEST_F(LogFilterTest, UnknownLevelIsAlwaysShown) {
  LogFilter none;
  none.levels.fill(false);
  EXPECT_EQ(pass(none), (std::vector<double>{6.0}));
}

TEST_F(LogFilterTest, RevealedCountOverTheFilteredRows) {
  LogFilter node;
  node.node = "/a";
  std::vector<uint32_t> indices;
  CompiledFilter(node, store_).collect(store_, 0, store_.size(), indices);  // times 1, 2, 5
  EXPECT_EQ(log_viewer::revealedCount(store_, indices, 0.0), 0u);
  EXPECT_EQ(log_viewer::revealedCount(store_, indices, 2.0), 2u);
  EXPECT_EQ(log_viewer::revealedCount(store_, indices, 4.9), 2u);
  EXPECT_EQ(log_viewer::revealedCount(store_, indices, 5.0), 3u);
}

TEST(LogFilterText, CaseInsensitiveSubstring) {
  EXPECT_TRUE(log_viewer::containsCaseInsensitive("Hello World", "o w"));
  EXPECT_TRUE(log_viewer::containsCaseInsensitive("anything", ""));
  EXPECT_FALSE(log_viewer::containsCaseInsensitive("abc", "abcd"));
  EXPECT_TRUE(log_viewer::containsCaseInsensitive("ERROR: x", "error"));
}

}  // namespace
