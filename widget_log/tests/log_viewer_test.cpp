// Loads the built library through PJ::WidgetLibrary, as the host does, binds a fake widget host and a
// fake pj.playback.v1, and drives the panel on the offscreen platform.

#include <gtest/gtest.h>

#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <pj_plugins/host/widget_library.hpp>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <string>
#include <utility>
#include <vector>

#include "fake_log_host.hpp"

namespace {

using Level = PJ::sdk::Log::Level;
using log_viewer_test::FakeLogHost;
using log_viewer_test::makeLog;

/// pj.playback.v1 with a play flag and a record of the seeks.
struct FakePlayback {
  bool playing = false;
  double time = 0.0;
  std::vector<double> seeks;

  PJ_playback_host_t raw() {
    static const PJ_playback_host_vtable_t vtable = {
        1, sizeof(PJ_playback_host_vtable_t), &play, &pause, &seek, &setRate, &getState, &toDisplayTime, nullptr,
    };
    return PJ_playback_host_t{this, &vtable};
  }

  static bool play(void* ctx, PJ_error_t* /*err*/) noexcept {
    static_cast<FakePlayback*>(ctx)->playing = true;
    return true;
  }
  static bool pause(void* ctx, PJ_error_t* /*err*/) noexcept {
    static_cast<FakePlayback*>(ctx)->playing = false;
    return true;
  }
  static bool seek(void* ctx, double time_s, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakePlayback*>(ctx);
    self->seeks.push_back(time_s);
    self->time = time_s;
    return true;
  }
  static bool setRate(void* /*ctx*/, double /*rate*/, PJ_error_t* /*err*/) noexcept {
    return true;
  }
  static bool getState(void* ctx, PJ_playback_state_t* out, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakePlayback*>(ctx);
    *out = PJ_playback_state_t{self->playing, self->time, 0.0, 1000.0, 1.0};
    return true;
  }
  static bool toDisplayTime(
      void* /*ctx*/, PJ_string_view_t /*topic*/, int64_t /*ns*/, double* /*out*/, PJ_error_t* /*err*/) noexcept {
    return false;
  }
};

/// A bound panel and its parent, destroyed in the order the host uses: detach, then destroy.
struct Panel {
  explicit Panel(PJ::WidgetHandle owned) : handle(std::move(owned)) {}
  Panel(const Panel&) = delete;
  Panel& operator=(const Panel&) = delete;
  ~Panel() {
    if (widget != nullptr) {
      widget->setParent(nullptr);
    }
    handle.reset();
  }

  void update(double tracker) {
    playback.time = tracker;
    const auto values = host.values(tracker);
    handle.onUpdate(PJ::Span<const PJ_widget_value_t>(values.data(), values.size()), tracker);
    QApplication::processEvents();
  }

  [[nodiscard]] QTreeView* view() const {
    return widget->findChild<QTreeView*>("logView");
  }
  [[nodiscard]] QAbstractItemModel* model() const {
    return view()->model();
  }
  [[nodiscard]] bool rowVisible(int row) const {
    const QRect rect = view()->visualRect(model()->index(row, 0));
    return rect.isValid() && view()->viewport()->rect().intersects(rect);
  }
  [[nodiscard]] QString message(int row) const {
    return model()->index(row, 4).data().toString();
  }

  FakeLogHost host;
  FakePlayback playback;
  PJ::ServiceRegistryBuilder registry;
  PJ::WidgetHandle handle;
  QWidget parent;
  QWidget* widget = nullptr;
};

class LogViewerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto loaded = PJ::WidgetLibrary::load(PJ_LOG_VIEWER_PLUGIN_PATH);
    ASSERT_TRUE(loaded) << loaded.error();
    library_ = std::make_unique<PJ::WidgetLibrary>(std::move(*loaded));
  }

  /// A panel bound to the fake host (and to the fake playback when @p with_playback), not yet created.
  std::unique_ptr<Panel> bound(bool with_playback = true) {
    auto handle = library_->createHandle("log_viewer");
    EXPECT_TRUE(handle) << handle.error();
    if (!handle) {
      return nullptr;
    }
    auto panel = std::make_unique<Panel>(std::move(*handle));
    panel->registry.registerService<PJ::sdk::WidgetHostService>(panel->host.bridge.host());
    if (with_playback) {
      panel->registry.registerService<PJ::sdk::PlaybackHostService>(panel->playback.raw());
    }
    const auto status = panel->handle.bind(panel->registry.view());
    EXPECT_TRUE(status) << status.error();
    return panel;
  }

  static void realize(Panel& panel) {
    auto widget = panel.handle.createQWidget(&panel.parent);
    ASSERT_TRUE(widget) << widget.error();
    panel.widget = static_cast<QWidget*>(*widget);
    panel.parent.resize(700, 500);
    panel.widget->resize(700, 500);
    panel.parent.show();
    QApplication::processEvents();
  }

  std::unique_ptr<Panel> open(bool with_playback = true) {
    auto panel = bound(with_playback);
    if (panel) {
      realize(*panel);
    }
    return panel;
  }

  /// @p count entries on @p slot, one per second from @p start.
  static void fill(Panel& panel, uint32_t slot, const std::string& label, int count, double start = 0.0) {
    panel.host.wire(slot, label);
    for (int i = 0; i < count; ++i) {
      panel.host.add(slot, start + i, makeLog(Level::kInfo, label + " #" + std::to_string(i), "/node"));
    }
  }

  std::unique_ptr<PJ::WidgetLibrary> library_;
};

TEST_F(LogViewerTest, ManifestAndInputs) {
  const auto manifest = nlohmann::json::parse(library_->vtable()->manifest_json);
  EXPECT_EQ(manifest.at("id"), "log-viewer");
  EXPECT_EQ(manifest.at("min_sdk_required"), "0.41.0");
  ASSERT_EQ(manifest.at("widgets").size(), 1u);
  EXPECT_EQ(manifest.at("widgets")[0].at("type"), "log_viewer");
  EXPECT_EQ(manifest.at("widgets")[0].at("name"), "Log");

  const PJ_widget_build_info_t* info = library_->buildInfo();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->qt_major, static_cast<uint32_t>(PJ_EXPECTED_QT_MAJOR));
  EXPECT_EQ(info->qt_minor, static_cast<uint32_t>(PJ_EXPECTED_QT_MINOR));

  auto panel = open();
  ASSERT_NE(panel, nullptr);
  const auto inputs = panel->handle.inputs();
  ASSERT_TRUE(inputs) << inputs.error();
  ASSERT_EQ(inputs->size(), 1u);
  EXPECT_EQ((*inputs)[0].name, "logs");
  EXPECT_EQ((*inputs)[0].label, "Logs");
  EXPECT_EQ((*inputs)[0].kind, PJ::WidgetInputKind::kObject);
  EXPECT_TRUE((*inputs)[0].multiple);
}

TEST_F(LogViewerTest, EmptyStateAndTopicColumn) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  auto* pages = panel->widget->findChild<QStackedWidget*>("pages");
  auto* topics = panel->widget->findChild<QToolButton*>("topicsButton");
  ASSERT_NE(pages, nullptr);
  ASSERT_NE(topics, nullptr);
  panel->update(0.0);
  EXPECT_EQ(pages->currentWidget()->objectName(), "emptyPage");
  EXPECT_EQ(panel->widget->findChild<QLabel*>("emptyLabel")->text(), "Drop a log topic (e.g. /rosout) here");

  fill(*panel, 0, "/rosout", 3);
  panel->update(10.0);
  EXPECT_EQ(pages->currentWidget()->objectName(), "contentPage");
  EXPECT_TRUE(panel->view()->isColumnHidden(3));
  EXPECT_FALSE(topics->isVisibleTo(panel->widget));

  fill(*panel, 1, "/other/rosout", 2);
  panel->update(10.0);
  EXPECT_FALSE(panel->view()->isColumnHidden(3));
  EXPECT_TRUE(topics->isVisibleTo(panel->widget));
  EXPECT_EQ(panel->model()->rowCount(), 5);

  // Hiding a topic from the menu hides its rows.
  ASSERT_EQ(topics->menu()->actions().size(), 2);
  topics->menu()->actions()[1]->setChecked(false);
  EXPECT_EQ(panel->model()->rowCount(), 3);
}

TEST_F(LogViewerTest, NothingInThePanelTakesDrops) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  EXPECT_FALSE(panel->widget->acceptDrops());
  EXPECT_FALSE(panel->view()->acceptDrops());
  EXPECT_FALSE(panel->view()->viewport()->acceptDrops());
  EXPECT_FALSE(panel->widget->findChild<QLineEdit*>("filterEdit")->acceptDrops());
  auto* detail = panel->widget->findChild<QPlainTextEdit*>("detailText");
  EXPECT_FALSE(detail->acceptDrops());
  EXPECT_FALSE(detail->viewport()->acceptDrops());
}

TEST_F(LogViewerTest, PlayingFollowsTheTrackerAndBlocksScrolling) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  fill(*panel, 0, "/rosout", 500);
  panel->playback.playing = true;
  panel->update(250.0);
  ASSERT_EQ(panel->model()->rowCount(), 500);
  EXPECT_TRUE(panel->rowVisible(250));
  EXPECT_FALSE(panel->rowVisible(0));
  QScrollBar* bar = panel->view()->verticalScrollBar();
  EXPECT_FALSE(bar->isEnabled());
  const int before = bar->value();

  const QPoint centre = panel->view()->viewport()->rect().center();
  QWheelEvent wheel(
      centre, panel->view()->viewport()->mapToGlobal(centre), QPoint(), QPoint(0, -360), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, false);
  QApplication::sendEvent(panel->view()->viewport(), &wheel);
  QTest::keyClick(panel->view(), Qt::Key_PageDown);
  QTest::keyClick(panel->view(), Qt::Key_End);
  QApplication::processEvents();
  EXPECT_EQ(bar->value(), before);

  // The tracker advances: the view follows.
  panel->update(400.0);
  EXPECT_TRUE(panel->rowVisible(400));
  EXPECT_GT(bar->value(), before);

  // Paused: scrolling is free again.
  panel->playback.playing = false;
  panel->update(400.0);
  EXPECT_TRUE(bar->isEnabled());
  const int paused = bar->value();
  QApplication::sendEvent(panel->view()->viewport(), &wheel);
  QApplication::processEvents();
  EXPECT_NE(bar->value(), paused);
}

TEST_F(LogViewerTest, PausedClickSeeksToTheRowAndKeepsTheView) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  fill(*panel, 0, "/rosout", 500);
  panel->update(300.0);
  QTreeView* view = panel->view();
  view->scrollTo(panel->model()->index(100, 0), QAbstractItemView::PositionAtTop);
  QApplication::processEvents();
  const int scrolled = view->verticalScrollBar()->value();
  ASSERT_TRUE(panel->rowVisible(105));

  const QRect rect = view->visualRect(panel->model()->index(105, 4));
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  ASSERT_EQ(panel->playback.seeks.size(), 1u);
  EXPECT_DOUBLE_EQ(panel->playback.seeks[0], 105.0);

  // The host answers the seek with an update at the new time: the view stays on the clicked row.
  panel->update(panel->playback.seeks[0]);
  EXPECT_EQ(view->verticalScrollBar()->value(), scrolled);
  EXPECT_TRUE(panel->rowVisible(105));
  EXPECT_EQ(view->currentIndex().row(), 105);
  auto* detail = panel->widget->findChild<QPlainTextEdit*>("detailText");
  EXPECT_TRUE(detail->toPlainText().contains("/rosout #105"));

  // A tracker move from elsewhere brings the last revealed row into view.
  panel->update(450.0);
  EXPECT_TRUE(panel->rowVisible(450));
}

TEST_F(LogViewerTest, ClickedRowStaysPutAmongEqualTimes) {
  // 60 entries share t = 100: after a click on the first, following the tracker would show the last of
  // them and scroll the clicked row away.
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  fill(*panel, 0, "/rosout", 100);
  for (int i = 0; i < 60; ++i) {
    panel->host.add(0, 100.0, makeLog(Level::kInfo, "burst #" + std::to_string(i)));
  }
  for (int i = 0; i < 100; ++i) {
    panel->host.add(0, 101.0 + i, makeLog(Level::kInfo, "after #" + std::to_string(i)));
  }
  panel->update(250.0);
  QTreeView* view = panel->view();
  view->scrollTo(panel->model()->index(100, 0), QAbstractItemView::PositionAtTop);
  QApplication::processEvents();
  const int scrolled = view->verticalScrollBar()->value();
  ASSERT_FALSE(panel->rowVisible(159));

  const QRect rect = view->visualRect(panel->model()->index(100, 4));
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  ASSERT_EQ(panel->playback.seeks.size(), 1u);
  EXPECT_DOUBLE_EQ(panel->playback.seeks[0], 100.0);
  panel->update(panel->playback.seeks[0]);
  EXPECT_EQ(view->verticalScrollBar()->value(), scrolled);
  EXPECT_TRUE(panel->rowVisible(100));
}

TEST_F(LogViewerTest, WithoutPlaybackServiceClickDoesNothing) {
  auto panel = open(/*with_playback=*/false);
  ASSERT_NE(panel, nullptr);
  fill(*panel, 0, "/rosout", 50);
  panel->playback.playing = true;  // not bound: must not matter
  panel->update(20.0);
  QTreeView* view = panel->view();
  EXPECT_TRUE(view->verticalScrollBar()->isEnabled());
  view->scrollTo(panel->model()->index(3, 4));  // the list is shorter now that the detail pane is taller
  const QRect rect = view->visualRect(panel->model()->index(3, 4));
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  EXPECT_TRUE(panel->playback.seeks.empty());
  EXPECT_EQ(view->currentIndex().row(), 3);
}

TEST_F(LogViewerTest, NonLogWireIsReported) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  auto& wire = panel->host.wire(0, "/camera/image");
  wire.type_name = "kImage";
  panel->host.addBytes(0, 1.0, {1, 2, 3});
  panel->update(2.0);
  panel->update(3.0);
  ASSERT_EQ(panel->host.warnings.size(), 1u);
  EXPECT_EQ(panel->host.warnings[0], "/camera/image is not a log topic");
  EXPECT_EQ(panel->model()->rowCount(), 0);
}

TEST_F(LogViewerTest, FutureRowsGreyOrHidden) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  fill(*panel, 0, "/rosout", 100);
  panel->update(49.5);
  QAbstractItemModel* model = panel->model();
  EXPECT_EQ(model->rowCount(), 100);
  const QColor grey = panel->view()->palette().color(QPalette::PlaceholderText);
  EXPECT_EQ(model->index(60, 4).data(Qt::ForegroundRole).value<QColor>(), grey);
  EXPECT_FALSE(model->index(40, 4).data(Qt::ForegroundRole).isValid());
  // Only the row the tracker is at (the last one at or before it) is tinted, and it follows the tracker.
  EXPECT_TRUE(model->index(49, 0).data(Qt::BackgroundRole).isValid());
  EXPECT_FALSE(model->index(48, 0).data(Qt::BackgroundRole).isValid());
  EXPECT_FALSE(model->index(50, 0).data(Qt::BackgroundRole).isValid());
  QSignalSpy repaint(model, &QAbstractItemModel::dataChanged);
  panel->update(52.5);
  EXPECT_TRUE(model->index(52, 0).data(Qt::BackgroundRole).isValid());
  EXPECT_FALSE(model->index(49, 0).data(Qt::BackgroundRole).isValid());
  const auto repainted = [&repaint](int row) {
    return std::any_of(repaint.begin(), repaint.end(), [row](const QList<QVariant>& args) {
      return args[0].toModelIndex().row() <= row && row <= args[1].toModelIndex().row() &&
             args[2].value<QList<int>>().contains(Qt::BackgroundRole);
    });
  };
  EXPECT_TRUE(repainted(49));
  EXPECT_TRUE(repainted(52));
  panel->update(49.5);

  ASSERT_TRUE(panel->handle.loadConfig(R"({"future_rows":"hide"})"));
  EXPECT_EQ(model->rowCount(), 50);
  panel->update(80.0);
  EXPECT_EQ(model->rowCount(), 81);
  EXPECT_TRUE(model->index(80, 0).data(Qt::BackgroundRole).isValid());
  EXPECT_FALSE(model->index(49, 0).data(Qt::BackgroundRole).isValid());
  panel->update(10.0);
  EXPECT_EQ(model->rowCount(), 11);
  EXPECT_EQ(panel->message(10), "/rosout #10");

  // New entries after the tracker stay hidden; at or before it they appear.
  panel->host.add(0, 100.0, makeLog(Level::kInfo, "late"));
  panel->update(10.0);
  EXPECT_EQ(model->rowCount(), 11);
  panel->update(200.0);
  EXPECT_EQ(model->rowCount(), 101);

  EXPECT_FALSE(panel->handle.loadConfig(R"({"future_rows":"blink"})"));
}

TEST_F(LogViewerTest, LevelColoursAndFirstLine) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  panel->host.wire(0, "/rosout");
  panel->host.add(0, 1.0, makeLog(Level::kInfo, "fine"));
  panel->host.add(0, 2.0, makeLog(Level::kWarning, "careful\nsecond line"));
  panel->host.add(0, 3.0, makeLog(Level::kError, "broken"));
  panel->update(5.0);
  QAbstractItemModel* model = panel->model();
  EXPECT_FALSE(model->index(0, 1).data(Qt::ForegroundRole).isValid());
  const QColor warn = model->index(1, 1).data(Qt::ForegroundRole).value<QColor>();
  const QColor error = model->index(2, 1).data(Qt::ForegroundRole).value<QColor>();
  EXPECT_TRUE(warn.isValid());
  EXPECT_TRUE(error.isValid());
  EXPECT_NE(warn, error);
  EXPECT_EQ(panel->message(1), "careful");
  EXPECT_EQ(model->index(1, 1).data().toString(), "Warn");
}

TEST_F(LogViewerTest, FiltersFromTheToolbar) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  panel->host.wire(0, "/rosout");
  panel->host.add(0, 1.0, makeLog(Level::kDebug, "motor spin", "/a"));
  panel->host.add(0, 2.0, makeLog(Level::kInfo, "Motor ready", "/b"));
  panel->host.add(0, 3.0, makeLog(Level::kError, "battery", "/a"));
  panel->host.add(0, 4.0, makeLog(Level::kUnknown, "plain", "/b"));
  panel->update(10.0);
  QAbstractItemModel* model = panel->model();
  ASSERT_EQ(model->rowCount(), 4);

  panel->widget->findChild<QToolButton*>("levelDebug")->setChecked(false);
  EXPECT_EQ(model->rowCount(), 3);

  auto* nodes = panel->widget->findChild<QComboBox*>("nodeCombo");
  ASSERT_EQ(nodes->count(), 3);  // All nodes, /a, /b
  nodes->setCurrentIndex(nodes->findData(QStringLiteral("/b")));
  EXPECT_EQ(model->rowCount(), 2);

  // The text filter waits for typing to pause.
  auto* text = panel->widget->findChild<QLineEdit*>("filterEdit");
  text->setText("MOTOR");
  EXPECT_EQ(model->rowCount(), 2);
  QTest::qWait(400);
  EXPECT_EQ(model->rowCount(), 1);
  EXPECT_EQ(panel->message(0), "Motor ready");
}

TEST_F(LogViewerTest, ConfigRoundTrip) {
  auto panel = bound();
  ASSERT_NE(panel, nullptr);
  const nlohmann::json config = {
      {"future_rows", "hide"},
      {"levels", {"info", "warn", "fatal"}},
      {"node_filter", "/talker"},
      {"text_filter", "motor"},
      {"hidden_topics", {"/b"}},
      {"splitter_sizes", {300, 120}},
      {"column_widths", {100, 70, 150, 160}},
  };
  // Before the widget exists, as the host restores a layout; then after creation the state is shown.
  ASSERT_TRUE(panel->handle.loadConfig(config.dump()));
  realize(*panel);

  std::string saved;
  ASSERT_TRUE(panel->handle.saveConfig(saved));
  const auto state = nlohmann::json::parse(saved);
  EXPECT_FALSE(state.contains("future_rows"));  // a setting: the host stores it
  for (const char* key : {"levels", "node_filter", "text_filter", "hidden_topics", "column_widths"}) {
    EXPECT_EQ(state.at(key), config.at(key)) << key;
  }
  EXPECT_EQ(panel->widget->findChild<QLineEdit*>("filterEdit")->text(), "motor");
  EXPECT_FALSE(panel->widget->findChild<QToolButton*>("levelDebug")->isChecked());
  EXPECT_EQ(panel->widget->findChild<QComboBox*>("nodeCombo")->currentData().toString(), "/talker");
  EXPECT_EQ(panel->view()->header()->sectionSize(0), 100);

  // Through the plugin's own config: the same object restores the same state.
  auto other = bound();
  ASSERT_NE(other, nullptr);
  realize(*other);
  ASSERT_TRUE(other->handle.loadConfig(saved));
  std::string again;
  ASSERT_TRUE(other->handle.saveConfig(again));
  const auto restored = nlohmann::json::parse(again);
  for (const char* key : {"levels", "node_filter", "text_filter", "hidden_topics", "column_widths"}) {
    EXPECT_EQ(restored.at(key), state.at(key)) << key;
  }

  EXPECT_FALSE(panel->handle.loadConfig(R"({"levels":"info"})"));
  EXPECT_FALSE(panel->handle.loadConfig("not json"));
}

TEST_F(LogViewerTest, ShowsANoticeWhenTheHostCapsTheHistory) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  panel->host.hard_cap = 100;
  fill(*panel, 0, "/rosout", 250);
  panel->update(1000.0);
  auto* notice = panel->widget->findChild<QLabel*>("noticeLabel");
  EXPECT_TRUE(notice->isVisibleTo(panel->widget));
  EXPECT_EQ(notice->text(), "Showing 100 of 250 log entries");
  EXPECT_EQ(panel->model()->rowCount(), 100);
}

TEST_F(LogViewerTest, StreamingAppendsKeepTheSelection) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  fill(*panel, 0, "/rosout", 20);
  panel->update(100.0);
  panel->view()->setCurrentIndex(panel->model()->index(5, 0));
  for (int i = 20; i < 30; ++i) {
    panel->host.add(0, i, makeLog(Level::kInfo, "/rosout #" + std::to_string(i)));
  }
  panel->update(100.0);
  EXPECT_EQ(panel->model()->rowCount(), 30);
  EXPECT_EQ(panel->view()->currentIndex().row(), 5);
  EXPECT_EQ(panel->message(29), "/rosout #29");
}

TEST_F(LogViewerTest, DestroyBeforeUnloadIsClean) {
  {
    auto panel = open();
    ASSERT_NE(panel, nullptr);
    fill(*panel, 0, "/rosout", 10);
    panel->update(5.0);
  }
  {
    auto handle = library_->createHandle("log_viewer");
    ASSERT_TRUE(handle) << handle.error();
    auto parent = std::make_unique<QWidget>();
    ASSERT_TRUE(handle->createQWidget(parent.get()));
    parent.reset();
    handle->reset();
  }
  library_.reset();
}

// Run with --gtest_also_run_disabled_tests. Loads 170k entries through the plugin and filters them once.
TEST_F(LogViewerTest, DISABLED_Perf170kEntries) {
  auto panel = open();
  ASSERT_NE(panel, nullptr);
  panel->host.wire(0, "/rosout");
  constexpr int kCount = 170'000;
  for (int i = 0; i < kCount; ++i) {
    const auto level = static_cast<Level>(1 + i % 5);
    panel->host.add(
        0, i * 0.01,
        makeLog(
            level, "message number " + std::to_string(i) + (i % 7 == 0 ? " motor fault" : " all nominal"),
            "/node_" + std::to_string(i % 13), "file.cpp", "function", static_cast<uint32_t>(i % 500)));
  }
  QElapsedTimer timer;
  timer.start();
  panel->update(850.0);
  const qint64 load_ms = timer.elapsed();
  ASSERT_EQ(panel->model()->rowCount(), kCount);

  // Typing is debounced; toggling a level applies every filter at once, the typed text included, so it
  // times one full refilter with a text match.
  auto* text = panel->widget->findChild<QLineEdit*>("filterEdit");
  {
    const QSignalBlocker blocker(text);
    text->setText("MOTOR");
  }
  timer.restart();
  panel->widget->findChild<QToolButton*>("levelFatal")->setChecked(false);
  const qint64 filter_ms = timer.elapsed();
  int expected = 0;
  for (int i = 0; i < kCount; ++i) {
    expected += (i % 7 == 0 && i % 5 != 4) ? 1 : 0;
  }
  const int matches = panel->model()->rowCount();
  EXPECT_EQ(matches, expected);

  timer.restart();
  panel->update(500.0);
  const qint64 tracker_ms = timer.elapsed();

  std::printf(
      "[perf] load %d entries: %lld ms; level+text refilter: %lld ms (%d rows); tracker move: %lld ms\n", kCount,
      static_cast<long long>(load_ms), static_cast<long long>(filter_ms), matches, static_cast<long long>(tracker_ms));
}

}  // namespace

int main(int argc, char** argv) {
  qputenv(
      "QT_QPA_PLATFORM", qgetenv("QT_QPA_PLATFORM").isEmpty() ? QByteArray("offscreen") : qgetenv("QT_QPA_PLATFORM"));
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
