// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "core/types.h"
#include "flight/types.hpp"  // mosaico::SequenceInfo

namespace mosaico {

class FetchWorker;
class LuaQueryEngine;

struct SequenceRecord {
  std::string name;
  std::int64_t min_ts_ns = 0;
  std::int64_t max_ts_ns = 0;
  std::int64_t total_size_bytes = 0;
  Metadata metadata;
};

// DialogState — pure data the dialog state machine drives. Mutated on the
// GUI thread (from onWidgetEvent + worker-result slots routed via Qt's
// queued connections), serialized into WidgetData on every getWidgetData().
struct DialogState {
  std::mutex mu;
  std::string uri = "grpc+tls://demo.mosaico.dev:6726";
  bool connected = false;
  // True between a Connect click and the connectFinished result — drives
  // the Connect button's disabled state (PJ3 parity).
  bool connecting = false;
  // Set after a plaintext fallback has been attempted for the current
  // URI; prevents infinite TLS-fail → plaintext-fail → TLS-retry loop.
  bool attempted_plaintext_fallback = false;

  // Discovery
  std::vector<SequenceRecord> sequences;
  std::vector<std::string> sequence_names;  // mirrors sequences[i].name for fast scan
  std::vector<std::string> topic_names;
  std::vector<TopicInfo> topic_infos;  // partial info from listTopics (size/ts/created)
  // True between a sequence selection and its topicsReady — drives the
  // "Topics — loading…" header hint (the only in-panel feedback now that the
  // bottom status strip is gone; the topic-list RPC is a network round trip).
  bool topics_loading = false;
  // Full per-topic metadata (incl. Arrow schema) fetched on demand for the
  // Info panel, keyed by topic name. Survives across selection changes.
  std::map<std::string, TopicInfo, std::less<>> topic_meta;
  std::string seq_filter;
  std::string topic_filter;
  // Regex-mode toggles for the name filters (PJ3 ".*" buttons). Off = case-
  // insensitive substring; on = QRegularExpression match.
  bool seq_filter_regex = false;
  bool topic_filter_regex = false;
  // Column sort state — the plugin owns row ordering (built-in QTableWidget
  // sorting would desync the index-based selection/visibility). -1 = unsorted
  // (server/load order). seqTable cols: 0=Name 1=Date 2=Size; topicTable: 0=Name 1=Size.
  int seq_sort_col = -1;
  bool seq_sort_asc = true;
  int topic_sort_col = -1;
  bool topic_sort_asc = true;
  int seq_selected_row = -1;
  std::vector<int> topic_selected_rows;
  std::string selected_sequence;

  // Global timestamp span across all sequences, used to seed the date-range
  // edits and the "All" preset. Computed when sequences load.
  std::int64_t global_min_ts_ns = 0;
  std::int64_t global_max_ts_ns = 0;

  // Time range — RangeSlider handle positions in slider units [0, kSliderSteps],
  // applied proportionally to the selected sequence's [min_ts_ns, max_ts_ns].
  // Drives the qint64 start/end passed to pullTopic. PJ3 parity: kSliderSteps.
  static constexpr int kSliderSteps = 1'000'000;
  int range_lower = 0;
  int range_upper = kSliderSteps;

  // Sequence-level date filter: ISO-8601 strings driven by the
  // QDateTimeEdit pair. Empty = no filter on that side.
  std::string date_from_iso;
  std::string date_to_iso;
  // Latched epoch-ns of the picked range; recomputed on every change.
  qint64 date_from_ns = 0;
  qint64 date_to_ns = 0;
  // Time-of-day window (ms since midnight) + recurring-daily flag, for the
  // SequencePicker's "Every day" mode (PJ3 RangeFilter parity).
  int date_from_tod_ms = 0;
  int date_to_tod_ms = 86'399'999;
  bool date_every_day = false;

  // Suppress the error *notification* for an auto-connect (PJ3 AutoConnect
  // context shows no popup); explicit Connect clicks still report failures.
  bool suppress_connect_error = false;

  // Lua query bar. The host MetadataQueryBar owns the editor text, combos, and
  // completion (verbatim PJ3 QueryBar). The plugin pushes the persisted query
  // back ONCE on first widget_data (query_text_pushed) to restore it, then never
  // again — pushing every tick would clobber the widget's own edits (the bug
  // this design fixes). Edits flow back via onCodeChanged into query_text.
  std::string query_text;
  bool query_text_pushed = false;

  // Fetch progress — per-topic byte counters, refreshed from
  // pullProgress signals on the GUI thread.
  std::map<std::string, std::int64_t, std::less<>> bytes_by_topic;
  std::string fetch_status;

  // Fetch lifecycle (PJ3 parity: a batch of topics completes via
  // allFetchesComplete, not per-topic). The panel only closes after the
  // whole batch lands, and only when not cancelling.
  bool fetch_active = false;
  bool cancelling = false;
  int fetch_total = 0;   // topics requested this batch
  int fetch_done = 0;    // topics that reported pullFinished (ok or fail)
  int fetch_failed = 0;  // subset of fetch_done that failed
  bool imported_any = false;
  // Per-message error tally so identical failures collapse into "[Nx] msg"
  // (PJ3 showCopyableWarning dedup).
  std::map<std::string, int, std::less<>> error_counts;
  // Per-topic rolling speed samples: (epoch_ms, cumulative_bytes), trimmed to
  // a 5 s window — mirrors PJ3 DownloadStatsDialog speed calc.
  struct SpeedSample {
    std::int64_t ms;
    std::int64_t bytes;
  };
  std::map<std::string, std::vector<SpeedSample>, std::less<>> speed_samples;
  std::map<std::string, std::string, std::less<>> topic_fetch_status;  // name → "" / "Done" / "Failed"

  // Sub-dialog request flags (read+cleared in getWidgetData).
  bool open_cert_pending = false;

  // Staged credential edits from the cert sub-dialog. PanelEngine fires
  // onTextChanged for each QLineEdit/QCheckBox child after the user
  // clicks OK; we capture the values here until the synthetic
  // `subDialogAccepted` click commits them to QSettings under the
  // current URI.
  std::string pending_cert_path;
  std::string pending_api_key;
  bool pending_allow_insecure = false;
  bool has_pending_cert_edit = false;
  bool has_pending_api_key_edit = false;
  bool has_pending_allow_insecure_edit = false;

  // Close request (read+cleared in getWidgetData).
  bool close_pending = false;
};

class MosaicoDialog : public QObject, public PJ::DialogPluginTyped {
  Q_OBJECT
 public:
  MosaicoDialog();
  ~MosaicoDialog() override;

  // DialogPluginTyped overrides
  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  bool onClicked(std::string_view widget_name) override;
  bool onToggled(std::string_view widget_name, bool checked) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  bool onValueChanged(std::string_view widget_name, int value) override;
  bool onRangeChanged(std::string_view widget_name, int lower, int upper) override;
  bool onDateRangeChanged(
      std::string_view widget_name, std::string_view from_iso, std::string_view to_iso, bool every_day) override;
  bool onCodeChanged(std::string_view widget_name, std::string_view code) override;
  bool onQuerySelector(std::string_view widget_name, std::string_view role, std::string_view value) override;
  bool onHeaderClicked(std::string_view widget_name, int section) override;

  // Wires the toolbox host provider so the worker can ingest Arrow data
  // into the datastore on completion. Called by MosaicoToolbox after bind.
  void setHostProvider(std::function<PJ::sdk::ToolboxHostView()> provider);

  // Wires the runtime host provider so the dialog can fire
  // notifyDataChanged() after a successful import — the app uses this to
  // flush buffered writer chunks and refresh the catalog tree. Without
  // it, ingested topics never appear in the datasets panel.
  void setRuntimeHostProvider(std::function<PJ::ToolboxRuntimeHostView()> provider);

 private Q_SLOTS:
  void onConnectFinished(bool ok, QString status, QString error);
  void onSequencesReady(std::vector<SequenceInfo> sequences);
  // Progressive discovery (PJ3 parity): populate the table from the initial
  // list as soon as it arrives, then fill each row's Date/Size as the server
  // streams per-sequence detail (onSequenceInfoReady), before the final list.
  void onSequenceListStarted(std::vector<SequenceInfo> sequences);
  void onSequenceInfoReady(SequenceInfo sequence);
  void onTopicsReady(QString sequence_name, QStringList topic_names);
  void onTopicInfosReady(QString sequence_name, std::vector<TopicInfo> topics);
  void onTopicMetadataReady(QString sequence_name, QString topic_name, TopicInfo info);
  void onPullProgress(QString topic_name, qint64 bytes);
  void onPullFinished(QString sequence_name, QString topic_name, bool ok, QString error);
  void onAllFetchesComplete(QString sequence_name);

 private:
  // Forward a one-shot status/error message to the app's notification
  // dropdown via the runtime host (reportMessage). Safe no-op if the host
  // isn't bound yet.
  void notify(PJ::ToolboxMessageLevel level, const std::string& message);

  // Rebuild state_.sequences / sequence_names from a fresh listSequences
  // result. Caller MUST hold state_.mu. When seed_dates is true the date-range
  // picker is reseeded to the dataset's full [min,max] span (final result
  // only — the progressive early populate leaves the picker untouched).
  void populateSequencesLocked(std::vector<SequenceInfo>& sequences, bool seed_dates);

  // Re-order the sequence / topic row models per the current sort column+order
  // and re-map index-based selection by name. Caller MUST hold state_.mu;
  // both are no-ops when the table's sort column is -1 (load order).
  void sortSequencesLocked();
  void sortTopicsLocked();

  // Persist the Lua query + slider proportions to QSettings (PJ3 parity:
  // restored next time the panel opens). Caller must NOT hold state_.mu.
  void persistState();

  DialogState state_;
  QThread worker_thread_;
  FetchWorker* worker_;  // owned by worker_thread_ (deleteLater on finished)
  std::function<PJ::ToolboxRuntimeHostView()> runtime_host_provider_;
};

}  // namespace mosaico
