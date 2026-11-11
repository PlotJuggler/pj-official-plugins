// Table model over the filtered rows of a LogStore; the last row at or before the tracker is tinted.
#pragma once

#include <QAbstractTableModel>
#include <QPointer>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <functional>
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
  enum Column { kTime = 0, kLevel, kNode, kTopic, kMessage, kColumnCount };

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

  static QString levelName(Level level);
  /// The text of the Time column.
  static QString formatTime(double time);

 private:
  int visibleCount() const;
  void updateRevealed();
  /// Repaints the tracker's row after revealed_ moved from @p old_revealed.
  void refreshCurrentRow(size_t old_revealed);

  const LogStore& store_;
  std::function<QString(uint32_t)> topic_label_;
  QPointer<QWidget> palette_source_;
  LogFilter filter_;
  std::vector<uint32_t> filtered_;  ///< store indices that pass the filter, ascending
  size_t revealed_ = 0;             ///< prefix of filtered_ with time <= tracker_
  double tracker_ = 0.0;
  FutureRows future_rows_ = FutureRows::kGrey;
};

}  // namespace log_viewer
