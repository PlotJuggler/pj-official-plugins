// LogTableModel over a small store, so the eviction of the oldest rows is reachable. QAbstractItemModelTester
// checks every row signal against rowCount() and data(); its complaints arrive as warnings.

#include "log_table_model.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QGuiApplication>
#include <string>
#include <vector>

namespace {

using log_viewer::FutureRows;
using log_viewer::Level;
using log_viewer::LogFilter;
using log_viewer::LogRow;
using log_viewer::LogStore;
using log_viewer::LogTableModel;

std::vector<std::string> g_warnings;

void recordWarnings(QtMsgType type, const QMessageLogContext& /*context*/, const QString& message) {
  if (type != QtDebugMsg && type != QtInfoMsg) {
    g_warnings.push_back(message.toStdString());
  }
}

/// Rows at times first, first + 1, ...; odd times are Debug, so a filter without Debug keeps every other one.
std::vector<LogRow> rows(int first, int count, uint64_t& seq) {
  std::vector<LogRow> out;
  for (int t = first; t < first + count; ++t) {
    LogRow row;
    row.time = t;
    row.seq = seq++;
    row.level = t % 2 != 0 ? Level::kDebug : Level::kInfo;
    row.message = "t=" + std::to_string(t);
    out.push_back(std::move(row));
  }
  return out;
}

QString message(const LogTableModel& model, int row) {
  return model.data(model.index(row, LogTableModel::kMessage)).toString();
}

class LogTableModelEviction : public ::testing::TestWithParam<FutureRows> {
 protected:
  void SetUp() override {
    g_warnings.clear();
    previous_ = qInstallMessageHandler(recordWarnings);
    model_.setFutureRows(GetParam());
  }
  void TearDown() override {
    qInstallMessageHandler(previous_);
    for (const std::string& warning : g_warnings) {
      ADD_FAILURE() << warning;
    }
  }

  void insert(int first, int count) {
    model_.applyStoreChange(store_.insert(0, rows(first, count, seq_)));
    model_.setTracker(tracker_);
  }

  LogStore store_{5};
  LogTableModel model_{store_, nullptr, nullptr};
  QAbstractItemModelTester tester_{&model_, QAbstractItemModelTester::FailureReportingMode::Warning};
  QtMessageHandler previous_ = nullptr;
  uint64_t seq_ = 0;
  double tracker_ = 5.0;
};

TEST_P(LogTableModelEviction, EveryRowFollowsTheStore) {
  insert(1, 5);  // t=1..5, all revealed
  insert(6, 3);  // evicts t=1..3; t=6..8 are after the tracker
  ASSERT_EQ(store_.size(), 5u);
  EXPECT_EQ(model_.revealedRows(), 2);  // t=4, t=5
  EXPECT_EQ(model_.rowCount(), GetParam() == FutureRows::kGrey ? 5 : 2);
  EXPECT_EQ(message(model_, 0), "t=4");
  EXPECT_EQ(model_.storeIndex(0), 0u);
  EXPECT_EQ(model_.rowOf(1), 1);
}

TEST_P(LogTableModelEviction, FilteredRowsShiftByTheEvictedStoreRows) {
  LogFilter no_debug;
  no_debug.levels[static_cast<size_t>(Level::kDebug)] = false;
  model_.setFilter(no_debug);
  insert(1, 5);  // shown: t=2, 4
  tracker_ = 7.0;
  insert(6, 4);  // evicts t=1..4 (shown t=2, 4); store t=5..9, shown t=6, 8 (t=8 after the tracker)
  EXPECT_EQ(model_.revealedRows(), 1);
  EXPECT_EQ(model_.rowCount(), GetParam() == FutureRows::kGrey ? 2 : 1);
  EXPECT_EQ(message(model_, 0), "t=6");
  EXPECT_EQ(model_.storeIndex(0), 1u);  // t=5 is store row 0, and hidden
  EXPECT_EQ(model_.rowOf(0), -1);
}

INSTANTIATE_TEST_SUITE_P(
    FutureRowModes, LogTableModelEviction, ::testing::Values(FutureRows::kGrey, FutureRows::kHide),
    [](const ::testing::TestParamInfo<FutureRows>& param_info) {
      return param_info.param == FutureRows::kGrey ? std::string("Grey") : std::string("Hide");
    });

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
