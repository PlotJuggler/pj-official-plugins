// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <pj_base/sdk/descriptor_import.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <thread>
#include <vector>

#include "core/types.h"
#include "flight/types.hpp"  // mosaico::SequenceInfo
#include "promotion_settlement_gate.hpp"
#include "worker_types.h"  // ServerCredentials, ConnectResult, TopicRef, PullResultEvent

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
// GUI thread (from widget events + worker-result callbacks drained by onTick),
// serialized into WidgetData on every getWidgetData().
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
  // Bumped on every content/order change to `sequences` (populate + sort) so the
  // seqTable view cache below can detect staleness with a cheap counter compare.
  std::size_t seq_epoch = 0;
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
  // Regex-mode toggles for the name filters (PJ3 ".*" buttons). Off =
  // case-insensitive substring; on = standard regex match.
  bool seq_filter_regex = false;
  bool topic_filter_regex = false;
  // Column sort state — the plugin owns row ordering (built-in table widget
  // sorting would desync the index-based visibility filter). -1 = unsorted
  // (server/load order). seqTable cols: 0=Name 1=Date 2=Size; topicTable: 0=Name 1=Size.
  // PJ3 parity: both tables default to Name ascending (column 0) because the
  // server's iteration order is unstable — without a deterministic sort, rows
  // would visibly reshuffle on every re-fetch (sequence_panel.cpp:102,
  // topic_panel.cpp:101-104).
  int seq_sort_col = 0;
  bool seq_sort_asc = true;
  int topic_sort_col = 0;
  bool topic_sort_asc = true;
  // Selection is keyed by NAME, not row index: names survive the plugin-side
  // re-sorts and list refreshes that shuffle row positions (same text-keyed
  // convention as the mcap/ros2/webrtc pickers).
  std::vector<std::string> selected_topics;
  std::string selected_sequence;

  // Topic-selection mode, driven by the All|Custom radio pair in the Topics
  // column (radioTopicsAll / radioTopicsCustom -> DualOptionsWidget). true =
  // All: every listed topic downloads and the table rows are inert (disabled
  // per-row; the table itself stays enabled so it remains scrollable). false =
  // Custom: the selected rows download. `selected_topics` is preserved across
  // mode flips, so switching to All and back keeps the custom pick.
  bool topics_all = false;

  // Every Download is recorded in the persistent cache for layout re-import.
  // cache_directory: empty = the live standard resolution
  // (makeArtifactCache falls back to standardCacheRoot), shown as the
  // field's placeholder hint.
  std::string cache_directory;

  // PJ3 parity (main_window.cpp:1051-1052,1064-1065): the last sequence + topic
  // selection is persisted and re-selected on the next connect. These hold the
  // restored values until the matching sequence/topic list arrives; the
  // restore-on-arrival is one-shot (cleared once consumed) so a later manual
  // selection or re-fetch doesn't keep snapping back to the saved one.
  std::string restore_selected_sequence;
  std::vector<std::string> restore_selected_topics;

  // Global timestamp span across all sequences, used to seed the date-range
  // edits and the "All" preset. Computed when sequences load.
  std::int64_t global_min_ts_ns = 0;
  std::int64_t global_max_ts_ns = 0;

  // Time range — RangeSlider handle positions in slider units [0, kSliderSteps],
  // applied proportionally to the selected sequence's [min_ts_ns, max_ts_ns].
  // Drives the int64 start/end passed to pullTopic. PJ3 parity: kSliderSteps.
  static constexpr int kSliderSteps = 1'000'000;
  int range_lower = 0;
  int range_upper = kSliderSteps;

  // Sequence-level date filter: ISO-8601 strings driven by the
  // date picker pair. Empty = no filter on that side.
  std::string date_from_iso;
  std::string date_to_iso;
  // Latched epoch-ns of the picked range; recomputed on every change.
  std::int64_t date_from_ns = 0;
  std::int64_t date_to_ns = 0;

  // Suppress the error *notification* for an auto-connect (PJ3 AutoConnect
  // context shows no popup); explicit Connect clicks still report failures.
  bool suppress_connect_error = false;

  // Lua query editor. The query language lives entirely in the plugin; the host
  // shows a plain QPlainTextEdit (code-edit mode). The plugin pushes the
  // persisted query back ONCE on first widget_data (query_text_pushed) to
  // restore it, then never again — pushing every tick would clobber the editor's
  // own edits (the bug this design fixes). Edits flow back via onCodeChanged.
  std::string query_text;
  bool query_text_pushed = false;
  // Caret offset (bytes) in the query editor, delivered by onCodeChangedWithCursor.
  int query_cursor = 0;
  // Set when the plugin programmatically rewrites query_text (the + clause button)
  // and the new text+caret must be pushed back to the editor on the next tick.
  // User keystrokes do NOT set this, so the editor keeps owning its own text.
  bool query_push_pending = false;

  // Staged clause being assembled in ADD mode (lua.txt 1-3,9): the dropdown
  // picks accumulate here and reach the editor ONLY when PLUS commits them
  // (commitClause). Empty == that slot not yet picked. Cleared after a commit,
  // and ignored while the caret is ON a token (REPLACE mode edits in place).
  // staged_op empty == the "==" default (shown but not yet explicitly chosen).
  std::string staged_key;
  std::string staged_op;
  std::string staged_value;

  // Metadata schema (key → distinct values) for the query assist, cached by
  // seq_epoch so it rebuilds only when the sequence set changes.
  Schema query_schema;
  std::size_t query_schema_epoch = static_cast<std::size_t>(-1);

  // Memoized seqTable view (rows + visible-row set). widget_data() runs on the
  // GUI thread every host tick (~20Hz); recomputing the per-sequence display
  // strings, the metadata-map copies, and the name/date/Lua filter on every
  // tick burned the GUI thread and made the sequence list feel laggy. The
  // result is a pure function of the inputs captured here, so it is recomputed
  // only when one of them changes (PJ3 was event-driven, not timer-driven).
  struct SeqViewCache {
    bool valid = false;
    std::size_t seq_epoch = 0;
    std::string seq_filter;
    bool seq_filter_regex = false;
    std::string query_text;
    std::int64_t date_from_ns = 0;
    std::int64_t date_to_ns = 0;
    std::vector<std::vector<std::string>> rows;
    std::vector<int> visible;
  };
  SeqViewCache seq_view_cache;

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
  // onTextChanged for each text/checkable child after the user
  // clicks OK; we capture the values here until the synthetic
  // `subDialogAccepted` click commits them to persisted settings under the
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

class MosaicoDialog : public PJ::DialogPluginTyped {
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
  bool onFolderSelected(std::string_view widget_name, std::string_view path) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  bool onValueChanged(std::string_view widget_name, int value) override;
  bool onRangeChanged(std::string_view widget_name, int lower, int upper) override;
  bool onDateRangeChanged(std::string_view widget_name, std::string_view from_iso, std::string_view to_iso) override;
  bool onCodeChangedWithCursor(std::string_view widget_name, std::string_view code, int cursor) override;
  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onHeaderClicked(std::string_view widget_name, int section) override;
  bool onTick() override;

  // Wires the toolbox host provider so the worker can ingest Arrow data
  // into the datastore on completion. Called by MosaicoToolbox after bind.
  void setHostProvider(std::function<PJ::sdk::ToolboxHostView()> provider);

  // Wires the runtime host provider so the dialog can fire
  // notifyDataChanged() after a successful import — the app uses this to
  // flush buffered writer chunks and refresh the catalog tree. Without
  // it, ingested topics never appear in the datasets panel.
  void setRuntimeHostProvider(std::function<PJ::ToolboxRuntimeHostView()> provider);

  // Wires the host's pj.source_promotion.v1 view so a completed Download can
  // be recorded as a cache artifact and promoted to a file-backed dataset
  // (the layout re-import path). Optional — unset, every fetch stays
  // eager-only. Called by MosaicoToolbox during bind().
  void setPromotionProvider(std::function<PJ::SourcePromotionHostView()> provider);

  // Binds the host's `pj.settings.v1` store and restores persisted UI state
  // (+ auto-connect). Called by MosaicoToolbox during bind(). An unbound view
  // (host omits the optional service) yields defaults gracefully.
  void setSettings(PJ::sdk::SettingsView settings);

 private:
  // Restore persisted query/range/server + auto-connect. Runs once when the
  // settings view is bound, before the tick loop.
  void initFromSettings();

  // The shared cancel presentation (in-panel Cancel button and host-side
  // Stop): progress header + per-topic "Cancelling…" rows. Caller holds
  // state_.mu.
  void markFetchCancellingLocked();

  void onConnectFinished(ConnectResult result);
  void onSequencesReady(std::vector<SequenceInfo> sequences);
  // Progressive discovery (PJ3 parity): populate the table from the initial
  // list as soon as it arrives, then fill each row's Date/Size as the server
  // streams per-sequence detail (onSequenceInfoReady), before the final list.
  void onSequenceListStarted(std::vector<SequenceInfo> sequences);
  void onSequenceInfoReady(SequenceInfo sequence);
  void onTopicsReady(std::string sequence_name, std::vector<std::string> topic_names);
  void onTopicInfosReady(std::string sequence_name, std::vector<TopicInfo> topics);
  void onTopicMetadataReady(TopicRef topic, TopicInfo info);
  void onPullProgress(std::string topic_name, std::int64_t bytes);
  void onPullFinished(PullResultEvent result);
  void onAllFetchesComplete(std::string sequence_name);
  // Mark a mid-fetch reveal as wanted and try to fire it (see the
  // reveal_pending_ member doc). Call with state_.mu NOT held.
  void requestProgressiveReveal();
  // Fire the pending reveal unless the throttle window is still open or the
  // batch is ending (cancelling / last topic done — the terminal notify owns
  // those). Stamps last_progressive_notify_ AFTER the synchronous host refresh
  // returns, so the throttle measures GUI breathing room between rebuilds, not
  // event arrival gaps. Call with state_.mu NOT held.
  void maybeFireProgressiveReveal();

  void workerLoop();
  void postCommand(std::function<void()> fn);
  void postEvent(std::function<void()> fn);

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

  // Rebuild the cached query-assist metadata schema when the sequence set
  // changed (keyed by seq_epoch). Caller must hold state_.mu.
  void ensureQuerySchemaLocked();

  // Re-apply the persisted topic selection (restore_selected_topics) onto the
  // freshly-listed topic rows of the restored sequence. One-shot: clears the
  // restore slot once consumed. Returns the restored topic names that still need
  // an on-demand metadata fetch (Info panel) — the caller posts those after
  // releasing the lock. Caller MUST hold state_.mu.
  std::vector<std::string> restoreSelectedTopicsLocked();

  // Persist the Lua query + slider proportions to settings (PJ3 parity:
  // restored next time the panel opens). Caller must NOT hold state_.mu.
  void persistState();

  DialogState state_;
  // The host may retain a promotion result closure beyond dialog teardown.
  // This shared barrier is invalidated before any other destructor work.
  std::shared_ptr<PromotionSettlementGate> promotion_settlement_gate_;
  std::thread worker_thread_;
  std::unique_ptr<FetchWorker> worker_;
  std::mutex cmd_mu_;
  std::condition_variable cmd_cv_;
  std::deque<std::function<void()>> cmd_queue_;
  bool worker_stop_ = false;
  std::mutex evt_mu_;
  std::deque<std::function<void()>> evt_queue_;
  std::function<PJ::ToolboxRuntimeHostView()> runtime_host_provider_;
  // Progressive-reveal machinery (GUI-thread only): mid-fetch, each completed
  // topic requests a throttled notifyDataChanged() so it appears in the
  // catalog without waiting for the batch. reveal_pending_ marks a request
  // suppressed by the throttle; onTick retries it once the window passes, so
  // visibility stays bounded (~kMinRevealInterval) even when the next topic
  // completion is minutes away. onAllFetchesComplete's terminal notify is
  // unconditional and clears the flag.
  static constexpr std::chrono::milliseconds kMinRevealInterval{500};
  std::chrono::steady_clock::time_point last_progressive_notify_{};
  bool reveal_pending_ = false;
  // Host-backed QSettings-like store (pj.settings.v1). Default-constructed
  // unbound until setSettings(); an unbound view reads defaults / drops writes.
  PJ::sdk::SettingsView settings_;
};

}  // namespace mosaico
