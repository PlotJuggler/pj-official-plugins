// Table model over the filtered rows of a LogStore; the last row at or before the tracker is tinted.
#pragma once

#include <QAbstractTableModel>
#include <QPointer>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "log_filter.hpp"
#include "log_store.hpp"

namespace log_viewer {

/// What to do with rows later than the tracker.
enum class FutureRows { kGrey, kHide };

class LogTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { kTime = 0, kLevel, kSource, kCategory, kTopic, kMessage, kColumnCount };

  /// @p palette_source supplies the colours of greyed rows; @p topic_label names a wire.
  LogTableModel(
      const LogStore& store, std::function<QString(uint32_t wire)> topic_label, QWidget* palette_source,
      QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  /// Re-run the filter over the whole store.
  void setFilter(const LogFilter& filter);
  /// Follow a store mutation, re-filtering only the rows it touched. The rows it adds are not revealed
  /// until the next setTracker(), which the caller makes right after.
  void applyStoreChange(const StoreChange& change);
  void setTracker(double tracker);
  void setFutureRows(FutureRows mode);
  /// Time column as a local date and time with milliseconds (the default), or as display seconds.
  void setTimeAsDateTime(bool on);
  /// Per wire, what to add to a row's display time to get its absolute seconds (the host's display offset,
  /// undone for the date and time). A wire missing here adds nothing.
  void setWireOffsets(std::map<uint32_t, double> offsets);

  [[nodiscard]] FutureRows futureRows() const {
    return future_rows_;
  }
  /// Filtered rows with time <= tracker.
  [[nodiscard]] int revealedRows() const {
    return static_cast<int>(revealed_);
  }
  /// Store index of a model row.
  [[nodiscard]] std::optional<size_t> storeIndex(int row) const;
  /// Model row showing store index @p index, or -1.
  [[nodiscard]] int rowOf(size_t index) const;

  [[nodiscard]] bool timeAsDateTime() const {
    return time_as_datetime_;
  }
  [[nodiscard]] const std::map<uint32_t, double>& wireOffsets() const {
    return wire_offsets_;
  }
  /// The text of @p row's Time column.
  [[nodiscard]] QString timeText(const LogRow& row) const;

  static QString levelName(Level level);
  /// @p time in seconds, or with @p datetime as a local date and time with milliseconds when @p time is an
  /// instant (seconds since the epoch).
  static QString formatTime(double time, bool datetime = false);

 private:
  int visibleCount() const;
  void updateRevealed();
  /// Repaints the background of row @p row_end - 1 (the tracker's row when row_end rows are revealed).
  void repaintRowBefore(size_t row_end);

  const LogStore& store_;
  std::function<QString(uint32_t)> topic_label_;
  QPointer<QWidget> palette_source_;
  LogFilter filter_;
  std::vector<uint32_t> filtered_;  ///< store indices that pass the filter, ascending
  size_t revealed_ = 0;             ///< prefix of filtered_ with time <= tracker_
  /// Store rows evicted while applyStoreChange() signals the removals: filtered_ still holds the indices
  /// from before the eviction until then.
  size_t pending_drop_ = 0;
  double tracker_ = 0.0;
  FutureRows future_rows_ = FutureRows::kGrey;
  bool time_as_datetime_ = true;
  std::map<uint32_t, double> wire_offsets_;
};

}  // namespace log_viewer
