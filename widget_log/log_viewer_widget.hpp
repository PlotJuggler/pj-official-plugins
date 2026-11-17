// The log viewer panel: filter row, log list and detail pane.
#pragma once

#include <QTimer>
#include <QWidget>
#include <array>
#include <functional>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/span.hpp>
#include <string>
#include <vector>

#include "log_filter.hpp"
#include "log_loader.hpp"
#include "log_store.hpp"
#include "log_table_model.hpp"
#include "ui_log_viewer.h"

namespace log_viewer {

/// What the user set in the panel; persisted with the layout.
struct ViewState {
  std::array<bool, kToggleLevels.size()> levels = {true, true, true, true, true};
  std::string source;
  std::string category;
  std::string text;
  std::vector<std::string> hidden_topics;
  std::vector<int> splitter;  ///< empty = default sizes
  std::vector<int> columns;   ///< widths of every column but the last; empty = defaults
};

class LogViewerWidget : public QWidget {
  Q_OBJECT

 public:
  using WarningSink = std::function<void(const std::string& message)>;

  /// @p host must outlive the widget. @p playback is optional: without it the list always scrolls freely
  /// and a double click does not seek.
  LogViewerWidget(
      const PJ::WidgetHostView& host, std::optional<PJ::sdk::PlaybackHostView> playback, WarningSink warn,
      QWidget* parent = nullptr);

  void onUpdate(PJ::Span<const PJ_widget_value_t> values, double time);
  void setFutureRows(FutureRows mode);
  /// Time column (and detail pane) as a local date and time, the host's time offset undone, or as the
  /// display seconds.
  void setTimeAsDateTime(bool on);

  [[nodiscard]] ViewState state() const;
  void applyState(const ViewState& state);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  /// The level toggles, in kToggleLevels order.
  [[nodiscard]] std::array<QToolButton*, kToggleLevels.size()> levelToggles() const;
  [[nodiscard]] LogFilter currentFilter() const;
  [[nodiscard]] bool isPlaying() const;
  void applyFilters();
  void refreshTopics();
  void refreshSources();
  void refreshCategories();
  void refreshNotice();
  void updateDetail();
  void selectStoreRow(std::optional<size_t> store_index);
  [[nodiscard]] std::optional<size_t> selectedStoreRow() const;
  void scrollToTracker(bool to_bottom);
  void setScrollLocked(bool locked);
  void onRowDoubleClicked(const QModelIndex& index);
  /// Fit the Time and Level columns once, when rows first appear and the user has no widths of their own.
  void autoSizeColumns();
  /// Width of @p column that fits @p text and the column title.
  [[nodiscard]] int fitWidth(const QString& text, int column) const;
  /// Fit the Time column to the last row's text. False when there is no row to measure.
  bool fitTimeColumn();
  /// Ask the playback service for each wire's display offset, so the date and time is the entry's instant.
  void refreshWireOffsets();

  Ui::LogViewerForm ui_{};
  LogStore store_;
  LogLoader loader_{store_};
  LogTableModel* model_ = nullptr;
  const PJ::WidgetHostView& host_;
  std::optional<PJ::sdk::PlaybackHostView> playback_;
  WarningSink warn_;
  QTimer text_timer_;

  std::vector<std::string> hidden_topics_;
  std::vector<uint32_t> applied_hidden_wires_;
  size_t sources_listed_ = 0;
  size_t categories_listed_ = 0;
  std::vector<int> column_widths_;
  std::vector<int> pending_splitter_;
  bool splitter_sized_ = false;
  bool columns_fixed_ = false;  ///< saved or user-set widths exist: never auto-size

  /// The store row the detail pane shows (nullopt: none) and its text; valid while detail_valid_.
  bool detail_valid_ = false;
  std::optional<size_t> detail_row_;
  QString detail_text_;

  std::optional<double> tracker_;
  bool shown_rows_ = false;  ///< the list was positioned on the tracker once since it had rows
  std::optional<double> pending_seek_;
  bool locked_ = false;
};

}  // namespace log_viewer
