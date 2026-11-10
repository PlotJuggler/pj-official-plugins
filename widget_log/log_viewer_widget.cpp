#include "log_viewer_widget.hpp"

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace log_viewer {

namespace {

constexpr int kTextDebounceMs = 150;
const std::vector<int> kDefaultColumnWidths = {90, 60, 140, 140};

QString fromStd(const std::string& text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string toStd(const QString& text) {
  return text.toUtf8().toStdString();
}

QString formatStamp(int64_t ns) {
  const int64_t seconds = ns / 1'000'000'000;
  const int64_t fraction = std::llabs(ns % 1'000'000'000);
  const QString sign = (ns < 0 && seconds == 0) ? QStringLiteral("-") : QString();
  return QStringLiteral("%1%2.%3").arg(sign).arg(seconds).arg(fraction, 9, 10, QLatin1Char('0'));
}

bool sameTime(double a, double b) {
  return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(a));
}

bool isNavigationKey(int key) {
  switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
      return true;
    default:
      return false;
  }
}

}  // namespace

LogViewerWidget::LogViewerWidget(
    HistoryQuery query, std::optional<PJ::sdk::PlaybackHostView> playback, WarningSink warn, QWidget* parent)
    : QWidget(parent),
      query_(std::move(query)),
      playback_(std::move(playback)),
      warn_(std::move(warn)),
      column_widths_(kDefaultColumnWidths) {
  ui_.setupUi(this);
  // Drops of a topic must reach the host's dock, so nothing in the panel takes them.
  setAcceptDrops(false);
  ui_.logView->viewport()->setAcceptDrops(false);
  ui_.detailText->viewport()->setAcceptDrops(false);
  ui_.emptyLabel->setForegroundRole(QPalette::PlaceholderText);
  ui_.noticeLabel->setForegroundRole(QPalette::PlaceholderText);
  ui_.noticeLabel->hide();
  ui_.topicsButton->setMenu(new QMenu(ui_.topicsButton));
  ui_.topicsButton->hide();
  ui_.copyButton->setEnabled(false);
  ui_.nodeCombo->addItem(QStringLiteral("All nodes"), QString());
  ui_.splitter->setStretchFactor(0, 1);
  ui_.splitter->setStretchFactor(1, 0);
  ui_.detailText->setMinimumHeight(ui_.detailText->fontMetrics().lineSpacing() * 3);

  model_ = new LogTableModel(store_, [this](uint32_t wire) { return fromStd(loader_.label(wire)); }, ui_.logView, this);
  ui_.logView->setModel(model_);
  QHeaderView* header = ui_.logView->header();
  header->setSectionsMovable(false);
  header->setStretchLastSection(true);
  header->setSectionResizeMode(QHeaderView::Interactive);
  for (int column = 0; column < static_cast<int>(column_widths_.size()); ++column) {
    header->resizeSection(column, column_widths_[static_cast<size_t>(column)]);
  }
  ui_.logView->setColumnHidden(LogTableModel::kTopic, true);
  connect(header, &QHeaderView::sectionResized, this, [this](int column, int /*old*/, int size) {
    if (column >= 0 && column < static_cast<int>(column_widths_.size()) && size > 0) {
      column_widths_[static_cast<size_t>(column)] = size;
    }
  });

  ui_.logView->installEventFilter(this);
  ui_.logView->viewport()->installEventFilter(this);
  ui_.logView->verticalScrollBar()->installEventFilter(this);

  for (QToolButton* toggle : {ui_.levelDebug, ui_.levelInfo, ui_.levelWarn, ui_.levelError, ui_.levelFatal}) {
    connect(toggle, &QToolButton::toggled, this, &LogViewerWidget::applyFilters);
  }
  connect(ui_.nodeCombo, &QComboBox::currentIndexChanged, this, &LogViewerWidget::applyFilters);
  text_timer_.setSingleShot(true);
  text_timer_.setInterval(kTextDebounceMs);
  connect(&text_timer_, &QTimer::timeout, this, &LogViewerWidget::applyFilters);
  connect(ui_.filterEdit, &QLineEdit::textChanged, this, [this]() { text_timer_.start(); });
  connect(ui_.logView, &QAbstractItemView::clicked, this, &LogViewerWidget::onRowClicked);
  connect(ui_.logView->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &LogViewerWidget::updateDetail);
  connect(model_, &QAbstractItemModel::modelReset, this, &LogViewerWidget::updateDetail);
  connect(ui_.copyButton, &QPushButton::clicked, this, [this]() {
    QGuiApplication::clipboard()->setText(ui_.detailText->toPlainText());
  });
}

void LogViewerWidget::onUpdate(PJ::Span<const PJ_widget_value_t> values, double time) {
  SyncResult result = loader_.sync(values, query_);
  for (const std::string& warning : result.warnings) {
    if (warn_) {
      warn_(warning);
    }
  }
  if (result.wires_changed) {
    refreshTopics();
  }
  if (result.wires_changed && currentFilter().hidden_wires != applied_hidden_wires_) {
    applyFilters();  // a returning topic the user hid: its new rows must stay hidden
  } else {
    model_->applyStoreChange(result.change);
  }
  if (store_.nodes().size() != nodes_listed_) {
    refreshNodes();
  }
  refreshNotice();

  const bool moved = !have_tracker_ || !sameTime(time, tracker_);
  have_tracker_ = true;
  tracker_ = time;
  model_->setTracker(time);

  const bool playing = isPlaying();
  setScrollLocked(playing);
  if (playing) {
    pending_seek_.reset();
    scrollToTracker(true);
  } else if (moved) {
    // The tracker moved because of a click in this list: the clicked row stays where it is.
    const bool own_seek = pending_seek_.has_value() && sameTime(*pending_seek_, time);
    pending_seek_.reset();
    if (!own_seek) {
      scrollToTracker(false);
    }
  } else if (!shown_rows_ && model_->rowCount() > 0) {
    scrollToTracker(false);
  }
  if (model_->rowCount() > 0) {
    shown_rows_ = true;
  }
  updateDetail();
}

void LogViewerWidget::setFutureRows(FutureRows mode) {
  const std::optional<size_t> selected = selectedStoreRow();
  model_->setFutureRows(mode);
  selectStoreRow(selected);
}

bool LogViewerWidget::isPlaying() const {
  if (!playback_) {
    return false;
  }
  const auto state = playback_->state();
  return state && state->is_playing;
}

LogFilter LogViewerWidget::currentFilter() const {
  LogFilter filter;
  const std::array<const QToolButton*, kToggleLevels.size()> toggles = {
      ui_.levelDebug, ui_.levelInfo, ui_.levelWarn, ui_.levelError, ui_.levelFatal};
  for (size_t i = 0; i < kToggleLevels.size(); ++i) {
    filter.levels[static_cast<size_t>(kToggleLevels[i])] = toggles[i]->isChecked();
  }
  filter.node = toStd(ui_.nodeCombo->currentData().toString());
  filter.text = toStd(ui_.filterEdit->text().trimmed());
  for (const WireInfo& wire : loader_.wires()) {
    if (std::find(hidden_topics_.begin(), hidden_topics_.end(), wire.label) != hidden_topics_.end()) {
      filter.hidden_wires.push_back(wire.id);
    }
  }
  return filter;
}

void LogViewerWidget::applyFilters() {
  text_timer_.stop();
  const std::optional<size_t> selected = selectedStoreRow();
  const LogFilter filter = currentFilter();
  applied_hidden_wires_ = filter.hidden_wires;
  model_->setFilter(filter);
  selectStoreRow(selected);
  if (locked_) {
    scrollToTracker(true);
  } else if (const QModelIndex current = ui_.logView->currentIndex(); current.isValid()) {
    ui_.logView->scrollTo(current, QAbstractItemView::EnsureVisible);
  } else {
    scrollToTracker(false);
  }
  updateDetail();
}

std::optional<size_t> LogViewerWidget::selectedStoreRow() const {
  const QModelIndex current = ui_.logView->currentIndex();
  if (!current.isValid() || !ui_.logView->selectionModel()->isRowSelected(current.row(), QModelIndex())) {
    return std::nullopt;
  }
  return model_->storeIndex(current.row());
}

void LogViewerWidget::selectStoreRow(std::optional<size_t> store_index) {
  if (!store_index) {
    return;
  }
  const int row = model_->rowOf(*store_index);
  if (row < 0) {
    return;
  }
  const bool auto_scroll = ui_.logView->hasAutoScroll();
  ui_.logView->setAutoScroll(false);  // re-selecting must not move the view by itself
  ui_.logView->selectionModel()->setCurrentIndex(
      model_->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  ui_.logView->setAutoScroll(auto_scroll);
}

void LogViewerWidget::refreshTopics() {
  const std::vector<WireInfo> wires = loader_.wires();
  QMenu* menu = ui_.topicsButton->menu();
  menu->clear();
  for (const WireInfo& wire : wires) {
    QAction* action = menu->addAction(fromStd(wire.label));
    action->setCheckable(true);
    const bool hidden = std::find(hidden_topics_.begin(), hidden_topics_.end(), wire.label) != hidden_topics_.end();
    action->setChecked(!hidden);
    connect(action, &QAction::toggled, this, [this, label = wire.label](bool shown) {
      const auto found = std::find(hidden_topics_.begin(), hidden_topics_.end(), label);
      if (shown && found != hidden_topics_.end()) {
        hidden_topics_.erase(found);
      } else if (!shown && found == hidden_topics_.end()) {
        hidden_topics_.push_back(label);
      }
      applyFilters();
    });
  }
  const bool several = wires.size() > 1;
  ui_.topicsButton->setVisible(several);
  ui_.logView->setColumnHidden(LogTableModel::kTopic, !several);
  ui_.pages->setCurrentWidget(wires.empty() ? ui_.emptyPage : ui_.contentPage);
}

void LogViewerWidget::refreshNodes() {
  QStringList names;
  for (uint32_t id : store_.nodes()) {
    names.push_back(fromStd(store_.text(id)));
  }
  nodes_listed_ = store_.nodes().size();
  names.sort(Qt::CaseInsensitive);

  QComboBox* combo = ui_.nodeCombo;
  const QString current = combo->currentData().toString();
  const QSignalBlocker blocker(combo);
  combo->clear();
  combo->addItem(QStringLiteral("All nodes"), QString());
  for (const QString& name : names) {
    combo->addItem(name, name);
  }
  if (!current.isEmpty() && combo->findData(current) < 0) {
    combo->addItem(current, current);  // a restored choice whose node has not logged yet
  }
  combo->setCurrentIndex(std::max(0, combo->findData(current)));
}

void LogViewerWidget::refreshNotice() {
  const size_t received = loader_.receivedSamples();
  const size_t total = loader_.hostSamples();
  if (received < total) {
    ui_.noticeLabel->setText(QStringLiteral("Showing %1 of %2 log entries").arg(received).arg(total));
    ui_.noticeLabel->show();
  } else {
    ui_.noticeLabel->hide();
  }
}

void LogViewerWidget::updateDetail() {
  const std::optional<size_t> selected = selectedStoreRow();
  ui_.copyButton->setEnabled(selected.has_value());
  QString text;
  if (selected && *selected < store_.size()) {
    const LogRow& row = store_.row(*selected);
    QStringList lines;
    lines << QStringLiteral("Time: %1 s").arg(row.time, 0, 'f', 6);
    lines << QStringLiteral("Stamp: %1").arg(formatStamp(row.stamp_ns));
    lines << QStringLiteral("Level: %1").arg(LogTableModel::levelName(row.level));
    lines << QStringLiteral("Node: %1").arg(fromStd(store_.text(row.node)));
    lines << QStringLiteral("Topic: %1").arg(fromStd(loader_.label(row.wire)));
    const QString file = fromStd(store_.text(row.file));
    const QString function = fromStd(store_.text(row.function));
    if (!file.isEmpty() || !function.isEmpty()) {
      QString source = file;
      if (row.line > 0) {
        source += QStringLiteral(":%1").arg(row.line);
      }
      if (!function.isEmpty()) {
        source += source.isEmpty() ? function : QStringLiteral(" (%1)").arg(function);
      }
      lines << QStringLiteral("Source: %1").arg(source);
    }
    text = lines.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + fromStd(row.message);
  }
  if (text != ui_.detailText->toPlainText()) {
    ui_.detailText->setPlainText(text);
  }
}

void LogViewerWidget::scrollToTracker(bool to_bottom) {
  QTreeView* view = ui_.logView;
  const int last = model_->revealedRows() - 1;
  if (last < 0) {
    view->scrollToTop();
    return;
  }
  view->scrollTo(
      model_->index(last, 0), to_bottom ? QAbstractItemView::PositionAtBottom : QAbstractItemView::EnsureVisible);
}

void LogViewerWidget::setScrollLocked(bool locked) {
  if (locked == locked_) {
    return;
  }
  locked_ = locked;
  ui_.logView->verticalScrollBar()->setEnabled(!locked);
  ui_.logView->setAutoScroll(!locked);
}

bool LogViewerWidget::eventFilter(QObject* watched, QEvent* event) {
  if (locked_) {
    if (event->type() == QEvent::Wheel) {
      return true;
    }
    if (event->type() == QEvent::KeyPress && isNavigationKey(static_cast<QKeyEvent*>(event)->key())) {
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void LogViewerWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  // Absolute splitter sizes only stick once the splitter has its real geometry.
  if (!splitter_sized_) {
    splitter_sized_ = true;
    if (pending_splitter_.size() == 2) {
      ui_.splitter->setSizes(QList<int>(pending_splitter_.begin(), pending_splitter_.end()));
    } else {
      ui_.splitter->setSizes({height(), ui_.detailText->fontMetrics().lineSpacing() * 6});
    }
  }
}

void LogViewerWidget::onRowClicked(const QModelIndex& index) {
  if (!playback_ || !index.isValid() || isPlaying()) {
    return;
  }
  const std::optional<size_t> store_index = model_->storeIndex(index.row());
  if (!store_index || *store_index >= store_.size()) {
    return;
  }
  const double time = store_.row(*store_index).time;
  pending_seek_ = time;
  if (!playback_->seek(time)) {
    pending_seek_.reset();
  }
}

ViewState LogViewerWidget::state() const {
  ViewState state;
  const std::array<const QToolButton*, kToggleLevels.size()> toggles = {
      ui_.levelDebug, ui_.levelInfo, ui_.levelWarn, ui_.levelError, ui_.levelFatal};
  for (size_t i = 0; i < toggles.size(); ++i) {
    state.levels[i] = toggles[i]->isChecked();
  }
  state.node = toStd(ui_.nodeCombo->currentData().toString());
  state.text = toStd(ui_.filterEdit->text());
  state.hidden_topics = hidden_topics_;
  if (splitter_sized_) {
    const QList<int> sizes = ui_.splitter->sizes();
    state.splitter.assign(sizes.begin(), sizes.end());
  } else {
    state.splitter = pending_splitter_;
  }
  state.columns = column_widths_;
  return state;
}

void LogViewerWidget::applyState(const ViewState& state) {
  const std::array<QToolButton*, kToggleLevels.size()> toggles = {
      ui_.levelDebug, ui_.levelInfo, ui_.levelWarn, ui_.levelError, ui_.levelFatal};
  for (size_t i = 0; i < toggles.size(); ++i) {
    const QSignalBlocker blocker(toggles[i]);
    toggles[i]->setChecked(state.levels[i]);
  }
  {
    const QSignalBlocker blocker(ui_.nodeCombo);
    const QString node = fromStd(state.node);
    if (!node.isEmpty() && ui_.nodeCombo->findData(node) < 0) {
      ui_.nodeCombo->addItem(node, node);
    }
    ui_.nodeCombo->setCurrentIndex(std::max(0, ui_.nodeCombo->findData(node)));
  }
  {
    const QSignalBlocker blocker(ui_.filterEdit);
    ui_.filterEdit->setText(fromStd(state.text));
  }
  hidden_topics_ = state.hidden_topics;
  if (state.columns.size() == column_widths_.size()) {
    for (size_t column = 0; column < state.columns.size(); ++column) {
      if (state.columns[column] > 0) {
        ui_.logView->header()->resizeSection(static_cast<int>(column), state.columns[column]);
        column_widths_[column] = state.columns[column];
      }
    }
  }
  if (state.splitter.size() == 2) {
    pending_splitter_ = state.splitter;
    if (splitter_sized_) {
      ui_.splitter->setSizes(QList<int>(state.splitter.begin(), state.splitter.end()));
    }
  }
  refreshTopics();
  applyFilters();
}

}  // namespace log_viewer
