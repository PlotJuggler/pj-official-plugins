// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

// clang-format off
// Arrow headers must stay before the dialog header because the plugin data API
// and Arrow C-data declarations have load-bearing include order.
#include <arrow/api.h>

#include "mosaico_dialog.hpp"
// clang-format on

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <pj_base/sdk/platform.hpp>
#include <set>
#include <string_view>
#include <utility>

#include "cert_dialog_ui.hpp"
#include "core/time_format.h"
#include "date_filter.h"
#include "fetch_summary.h"
#include "fetch_worker.hpp"
#include "format_utils.h"
#include "mosaico_panel_manifest.hpp"
#include "mosaico_panel_ui.hpp"
#include "name_filter.h"
#include "query/edit.h"
#include "query/engine.h"
#include "query/query.h"
#include "query_filter.h"
#include "server_history.h"
#include "settings_store.hpp"
#include "table_sort.h"
#include "tls_utils.h"

namespace mosaico {

namespace {

struct ServerCredentials {
  std::string cert_path;
  std::string api_key;
  bool allow_insecure = false;
};

std::string credentialsSettingsPrefix(const std::string& uri) {
  return "mosaico/server_cache/" + normalizeServerKey(uri) + "/";
}

ServerCredentials loadCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri) {
  SettingsStore settings(view);
  const std::string prefix = credentialsSettingsPrefix(uri);
  ServerCredentials creds;
  creds.cert_path = settings.getString(prefix + "cert_path");
  creds.api_key = settings.getString(prefix + "api_key");
  creds.allow_insecure = settings.getBool(prefix + "allow_insecure", false);
  return creds;
}

void saveCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri, const ServerCredentials& creds) {
  SettingsStore settings(view);
  const std::string prefix = credentialsSettingsPrefix(uri);
  settings.setString(prefix + "cert_path", creds.cert_path);
  settings.setString(prefix + "api_key", creds.api_key);
  settings.setBool(prefix + "allow_insecure", creds.allow_insecure);
}

// Load per-server credentials, falling back to the MOSAICO_API_KEY environment
// variable when no key is cached for this server (PJ3 automation parity).
ServerCredentials resolveCredentials(PJ::sdk::SettingsView view, const std::string& uri) {
  ServerCredentials creds = loadCredentialsForUri(view, uri);
  if (creds.api_key.empty()) {
    if (auto env = PJ::sdk::getEnv("MOSAICO_API_KEY")) {
      creds.api_key = *env;
    }
  }
  return creds;
}

// ---------------------------------------------------------------------------
// Info-panel / table formatting helpers (ported from PJ3 data_view_panel.cpp
// and format_utils.h). The Info panel is rendered as monospaced plain text.
//
// formatBytes (1024-based, PJ3 parity) lives in src/format_utils.h so it can be
// unit-tested without the Arrow/Flight link.
// ---------------------------------------------------------------------------

std::string isoFromNs(std::int64_t ts_ns) {
  if (ts_ns <= 0) {
    return {};
  }
  return formatIso8601Utc(ts_ns);
}

std::string dateOnly(std::int64_t ts_ns) {
  if (ts_ns <= 0) {
    return "--/--/----";
  }
  return formatDateDDMMYYYY(ts_ns);
}

std::string dateTimeUtc(std::int64_t ts_ns) {
  return formatDateTimeUtc(ts_ns);
}

// --- Query-assist helpers (Key/Op/Value dropdowns) ---

// Clamp a (possibly stale or -1) caret offset into [0, len].
int clampQueryCursor(int cursor, const std::string& text) {
  const int n = static_cast<int>(text.size());
  return cursor < 0 ? n : (cursor > n ? n : cursor);
}

// Distinct metadata keys — the Schema map is already key-sorted.
std::vector<std::string> schemaKeys(const Schema& schema) {
  std::vector<std::string> keys;
  keys.reserve(schema.size());
  for (const auto& kv : schema) {
    keys.push_back(kv.first);
  }
  return keys;
}

// Distinct, sorted values recorded for `key` across the dataset.
std::vector<std::string> schemaValues(const Schema& schema, const std::string& key) {
  auto it = schema.find(key);
  if (it == schema.end()) {
    return {};
  }
  std::vector<std::string> values(it->second.begin(), it->second.end());
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

template <typename MapType>
std::string formatMetadata(const MapType& metadata, std::string_view indent = {}) {
  if (metadata.empty()) {
    return {};
  }
  // Deterministic ordering — unordered_map iteration order is unspecified.
  std::vector<std::pair<std::string, std::string>> sorted(metadata.begin(), metadata.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string text;
  for (const auto& [key, value] : sorted) {
    text += fmt::format("{}{}:\n{}  {}\n", indent, key, indent, value);
  }
  return text;
}

// Recursively format an Arrow field with vertical indentation for structs and
// lists, exactly like PJ3 data_view_panel::formatFieldType.
void formatFieldType(const std::shared_ptr<arrow::Field>& field, const std::string& indent, std::string& out) {
  auto type = field->type();
  if (type->id() == arrow::Type::STRUCT) {
    out += fmt::format("{}{}\n", indent, field->name());
    auto st = std::static_pointer_cast<arrow::StructType>(type);
    for (int i = 0; i < st->num_fields(); ++i) {
      formatFieldType(st->field(i), indent + "  ", out);
    }
  } else if (type->id() == arrow::Type::LIST) {
    auto inner = std::static_pointer_cast<arrow::ListType>(type)->value_field();
    out += fmt::format("{}{} []\n", indent, field->name());
    if (inner->type()->id() == arrow::Type::STRUCT) {
      auto st = std::static_pointer_cast<arrow::StructType>(inner->type());
      for (int i = 0; i < st->num_fields(); ++i) {
        formatFieldType(st->field(i), indent + "  ", out);
      }
    } else {
      out += fmt::format("{}  {}\n", indent, inner->type()->ToString());
    }
  } else {
    out += fmt::format("{}{} : {}\n", indent, field->name(), type->ToString());
  }
}

std::string formatSchemaFields(const std::shared_ptr<arrow::Schema>& schema) {
  std::string text;
  if (!schema) {
    return text;
  }
  text += fmt::format("Fields ({}):\n", schema->num_fields());
  for (int i = 0; i < schema->num_fields(); ++i) {
    formatFieldType(schema->field(i), "  ", text);
  }
  return text;
}

std::string buildSequenceInfoText(const SequenceRecord& rec) {
  std::string text;
  text += fmt::format("Sequence : {}\n", rec.name);
  if (rec.max_ts_ns > 0) {
    text += fmt::format("Date     : {}\n", dateOnly(rec.max_ts_ns));
  }
  if (rec.total_size_bytes > 0) {
    text += fmt::format("Size     : {}\n", formatBytes(rec.total_size_bytes));
  }
  if (!rec.metadata.empty()) {
    text += "\nMetadata:\n";
    text += formatMetadata(rec.metadata);
  }
  return text;
}

std::string buildTopicInfoText(const TopicInfo& info) {
  std::string text;
  text += fmt::format("Topic    : {}\n", info.topic_name);
  if (!info.ontology_tag.empty()) {
    text += fmt::format("Tag      : {}\n", info.ontology_tag);
  }
  if (info.created_at_ns > 0) {
    text += fmt::format("Created  : {}\n", dateTimeUtc(info.created_at_ns));
  }
  if (info.locked) {
    if (info.completed_at_ns.has_value() && *info.completed_at_ns > 0) {
      text += fmt::format("Status   : sealed ({})\n", dateTimeUtc(*info.completed_at_ns));
    } else {
      text += "Status   : sealed\n";
    }
  } else {
    text += "Status   : live\n";
  }
  if (info.chunks_number > 0) {
    text += fmt::format("Chunks   : {}\n", info.chunks_number);
  }
  if (info.total_size_bytes > 0) {
    text += fmt::format("Size     : {}\n", formatBytes(info.total_size_bytes));
  }
  if (!info.resource_locator.empty()) {
    text += fmt::format("Resource : {}\n", info.resource_locator);
  }
  if (info.schema) {
    text += formatSchemaFields(info.schema);
  }
  if (!info.user_metadata.empty()) {
    text += "\nMetadata:\n";
    text += formatMetadata(info.user_metadata);
  }
  return text;
}

}  // namespace

MosaicoDialog::MosaicoDialog() : worker_(std::make_unique<FetchWorker>()) {
  worker_->connectFinished = [this](bool ok, std::string status, std::string err) {
    postEvent([this, ok, status = std::move(status), err = std::move(err)]() mutable {
      onConnectFinished(ok, std::move(status), std::move(err));
    });
  };
  worker_->sequencesReady = [this](std::vector<SequenceInfo> sequences) {
    postEvent([this, sequences = std::move(sequences)]() mutable { onSequencesReady(std::move(sequences)); });
  };
  worker_->sequenceListStarted = [this](std::vector<SequenceInfo> sequences) {
    postEvent([this, sequences = std::move(sequences)]() mutable { onSequenceListStarted(std::move(sequences)); });
  };
  worker_->sequenceInfoReady = [this](SequenceInfo sequence) {
    postEvent([this, sequence = std::move(sequence)]() mutable { onSequenceInfoReady(std::move(sequence)); });
  };
  worker_->topicsReady = [this](std::string sequence_name, std::vector<std::string> topic_names) {
    postEvent([this, sequence_name = std::move(sequence_name), topic_names = std::move(topic_names)]() mutable {
      onTopicsReady(std::move(sequence_name), std::move(topic_names));
    });
  };
  worker_->topicInfosReady = [this](std::string sequence_name, std::vector<TopicInfo> topics) {
    postEvent([this, sequence_name = std::move(sequence_name), topics = std::move(topics)]() mutable {
      onTopicInfosReady(std::move(sequence_name), std::move(topics));
    });
  };
  worker_->topicMetadataReady = [this](std::string sequence_name, std::string topic_name, TopicInfo info) {
    postEvent([this, sequence_name = std::move(sequence_name), topic_name = std::move(topic_name),
               info = std::move(info)]() mutable {
      onTopicMetadataReady(std::move(sequence_name), std::move(topic_name), std::move(info));
    });
  };
  worker_->pullProgress = [this](std::string topic_name, std::int64_t bytes) {
    postEvent(
        [this, topic_name = std::move(topic_name), bytes]() mutable { onPullProgress(std::move(topic_name), bytes); });
  };
  worker_->pullFinished = [this](std::string sequence_name, std::string topic_name, bool ok, std::string error) {
    postEvent([this, sequence_name = std::move(sequence_name), topic_name = std::move(topic_name), ok,
               error = std::move(error)]() mutable {
      onPullFinished(std::move(sequence_name), std::move(topic_name), ok, std::move(error));
    });
  };
  worker_->allFetchesComplete = [this](std::string sequence_name) {
    postEvent(
        [this, sequence_name = std::move(sequence_name)]() mutable { onAllFetchesComplete(std::move(sequence_name)); });
  };
  worker_->errorOccurred = [this](std::string message) {
    postEvent([this, message = std::move(message)]() mutable { notify(PJ::ToolboxMessageLevel::kError, message); });
  };

  worker_thread_ = std::thread([this] { workerLoop(); });
  // Persisted-state restore + auto-connect happen in initFromSettings(), once
  // the host binds the settings view via setSettings() (during plugin bind()).
}

void MosaicoDialog::setSettings(PJ::sdk::SettingsView settings) {
  settings_ = settings;
  initFromSettings();
}

void MosaicoDialog::initFromSettings() {
  // Restore persisted UI state and auto-connect to the last server (PJ3
  // parity). Runs at bind time, before the tick loop or any worker result can
  // touch state_, so the unlocked access here is safe.
  SettingsStore settings(settings_);
  const std::vector<std::string> history = settings.getStringList("mosaico/server_history");
  if (!history.empty()) {
    state_.uri = history.front();
  }
  state_.query_text = settings.getString("mosaico/metadata_query");
  state_.range_lower = std::clamp(settings.getInt("mosaico/range_lower", 0), 0, DialogState::kSliderSteps);
  state_.range_upper =
      std::clamp(settings.getInt("mosaico/range_upper", DialogState::kSliderSteps), 0, DialogState::kSliderSteps);
  // PJ3 parity: stage the last selection for re-selection once the matching
  // sequence/topic list arrives from the auto-connect below.
  state_.restore_selected_sequence = settings.getString("mosaico/selected_sequence");
  state_.restore_selected_topics = settings.getStringList("mosaico/selected_topics");

  if (!history.empty()) {
    const std::string uri = state_.uri;
    const ServerCredentials creds = resolveCredentials(settings_, uri);
    // Same printable-ASCII gate as the explicit Connect handler (PJ3
    // connectToServer validates both paths). On auto-connect PJ3 shows no
    // popup, so a malformed persisted value silently aborts the auto-connect
    // rather than handing control bytes to gRPC.
    if (isPrintableAscii(uri) && isPrintableAscii(creds.cert_path) && isPrintableAscii(creds.api_key)) {
      state_.connecting = true;
      state_.suppress_connect_error = true;  // PJ3 AutoConnect: no error notification on failure.
      postCommand(
          [w = worker_.get(), uri, cert_path = creds.cert_path, api_key = creds.api_key,
           allow_insecure = creds.allow_insecure] { w->connectAsync(uri, cert_path, api_key, allow_insecure); });
    }
  }
}

MosaicoDialog::~MosaicoDialog() {
  // Persist the Lua query + slider proportions for next open (PJ3 parity).
  persistState();
  // Signal the SDK's mid-stream cancel flag before joining; otherwise the
  // worker could block indefinitely while pullTopics is in flight.
  if (worker_) {
    worker_->requestCancel();
  }
  {
    std::lock_guard<std::mutex> lock(cmd_mu_);
    worker_stop_ = true;
  }
  cmd_cv_.notify_all();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  // onTick and this destructor both run on the single GUI thread, so no
  // this-capturing event closure can run during or after destruction; closures
  // left in evt_queue_ are destroyed un-run with the dialog.
}

void MosaicoDialog::workerLoop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(cmd_mu_);
      cmd_cv_.wait(lock, [this] { return worker_stop_ || !cmd_queue_.empty(); });
      if (worker_stop_ && cmd_queue_.empty()) {
        return;
      }
      task = std::move(cmd_queue_.front());
      cmd_queue_.pop_front();
    }
    task();
  }
}

void MosaicoDialog::postCommand(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(cmd_mu_);
    cmd_queue_.push_back(std::move(fn));
  }
  cmd_cv_.notify_one();
}

void MosaicoDialog::postEvent(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(evt_mu_);
  evt_queue_.push_back(std::move(fn));
}

bool MosaicoDialog::onTick() {
  std::deque<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(evt_mu_);
    batch.swap(evt_queue_);
  }
  for (auto& c : batch) {
    try {
      c();
    } catch (...) {
      // Keep one bad callback from escaping the plugin tick ABI.
    }
  }
  return !batch.empty();
}

std::string MosaicoDialog::manifest() const {
  return kMosaicoPanelManifest;
}

std::string MosaicoDialog::ui_content() const {
  return kMosaicoPanelUi;
}

void MosaicoDialog::setHostProvider(std::function<PJ::sdk::ToolboxHostView()> provider) {
  worker_->setHostProvider(std::move(provider));
}

void MosaicoDialog::setRuntimeHostProvider(std::function<PJ::ToolboxRuntimeHostView()> provider) {
  runtime_host_provider_ = std::move(provider);
}

std::string MosaicoDialog::widget_data() {
  std::lock_guard<std::mutex> lock(state_.mu);
  PJ::WidgetData wd;
  // No in-panel status strip — connection / import / error events go to the
  // app's top notification bell via notify(); live download progress shows in
  // the Info panel during a fetch.
  wd.setText("comboUri", state_.uri);

  // PJ3 parity: combo always lists the MRU history + the demo server pin.
  {
    static const std::string kDemoServer = "grpc+tls://demo.mosaico.dev:6726";
    std::vector<std::string> items;
    const std::vector<std::string> history = SettingsStore(settings_).getStringList("mosaico/server_history");
    bool has_demo = false;
    for (const std::string& s : history) {
      items.push_back(s);
      if (s == kDemoServer) {
        has_demo = true;
      }
    }
    if (!has_demo) {
      items.push_back(kDemoServer);
    }
    wd.setItems("comboUri", items);
  }

  // Query editor (plain QPlainTextEdit, code-edit mode). The Lua query language
  // lives entirely in the plugin: we push the persisted text ONCE to restore it,
  // then validate edits via the plugin's engine and surface the result through
  // the generic field-validity indicator.
  {
    // One-shot text restore. Pushing query_text every tick would clobber the
    // editor's own edits the instant the user types (the stale-echo race that
    // broke the previous split design). After the first push the editor owns the
    // text; edits return via onCodeChanged.
    if (!state_.query_text_pushed || state_.query_push_pending) {
      wd.setCodeContent("lua_queryBar", state_.query_text);
      wd.setCodeCursor("lua_queryBar", state_.query_cursor);
      state_.query_text_pushed = true;
      state_.query_push_pending = false;
    }
    wd.setCodeLanguage("lua_queryBar", "lua");  // Lua syntax highlighting + code-editor wiring
    // Opt into caret tracking: the Key/Op/Value dropdowns re-analyze at the
    // caret, so we need cursor moves (not just edits) reported via
    // onCodeChangedWithCursor. Other code editors don't opt in and so aren't
    // re-run on every cursor move.
    wd.setCodeCaretTracking("lua_queryBar");

    // Validity feedback via the plugin's Lua engine. An empty query is valid (no
    // filter); otherwise the tick + tooltip reflect Engine::validate.
    if (state_.query_text.empty()) {
      wd.setFieldValid("lua_queryBar", true, "");
    } else {
      const bool valid = Engine::validate(state_.query_text).valid;
      wd.setFieldValid("lua_queryBar", valid, valid ? "" : "invalid syntax");
    }

    // Cursor-aware Key/Op/Value assist dropdowns. analyze() the query at the
    // caret to decide what each dropdown would insert/replace and whether it is
    // actionable; the value list is the distinct values of the key in context.
    // Each is reset to no-selection so picking the same item again re-fires.
    ensureQuerySchemaLocked();
    const int cursor = clampQueryCursor(state_.query_cursor, state_.query_text);
    const CursorContext ctx = analyze(state_.query_text, cursor, state_.query_schema);

    wd.setItems("keyCombo", schemaKeys(state_.query_schema));
    wd.setEnabled("keyCombo", ctx.can_pick_key());
    wd.setCurrentIndex("keyCombo", -1);

    wd.setItems("opCombo", operators());
    wd.setEnabled("opCombo", ctx.can_pick_op());
    wd.setCurrentIndex("opCombo", -1);

    wd.setItems("valCombo", schemaValues(state_.query_schema, ctx.context_key));
    wd.setEnabled("valCombo", ctx.can_pick_value());
    wd.setCurrentIndex("valCombo", -1);
  }

  // RangeSlider: bounds + handle values. Once a sequence with a known time
  // span is selected, enable it and turn on the duration floating labels
  // (handle = offset from start, center = selected duration) — PJ3 parity.
  wd.setRangeSliderBounds("rangeSlider", 0, DialogState::kSliderSteps);
  wd.setRangeSliderValues("rangeSlider", state_.range_lower, state_.range_upper);
  {
    const SequenceRecord* sel = nullptr;
    for (const auto& s : state_.sequences) {
      if (s.name == state_.selected_sequence) {
        sel = &s;
        break;
      }
    }
    if (sel != nullptr && sel->max_ts_ns > sel->min_ts_ns) {
      wd.setEnabled("rangeSlider", true);
      wd.setRangeSliderTimeSpan("rangeSlider", sel->min_ts_ns, sel->max_ts_ns);
    } else {
      wd.setEnabled("rangeSlider", false);
    }
  }

  // DateRangePicker: hint the dataset's available span as the from/to placeholders.
  if (state_.global_min_ts_ns > 0) {
    wd.setDateRangePlaceholder(
        "datePicker", formatDateOnlyIso(state_.global_min_ts_ns),
        state_.global_max_ts_ns > 0 ? formatDateOnlyIso(state_.global_max_ts_ns) : "");
  }

  // Button enable/disable (PJ3 parity): Connect is inert while a connect or a
  // fetch is in flight; Download needs a sequence + topic(s) and an idle
  // worker; Cancel is live only during a fetch.
  wd.setEnabled("buttonConnect", !state_.connecting && !state_.fetch_active);
  wd.setEnabled(
      "buttonFetch", state_.connected && !state_.selected_sequence.empty() && !state_.topic_selected_rows.empty() &&
                         !state_.fetch_active);
  // Refresh re-lists sequences without a disconnect/reconnect (PJ3
  // main_window.cpp:933-945): live only while connected and idle.
  wd.setEnabled("buttonRefresh", state_.connected && !state_.connecting && !state_.fetch_active);
  wd.setEnabled("buttonCancel", state_.fetch_active);
  // Closing mid-fetch tears the worker down before allFetchesComplete runs,
  // stranding the topics that already wrote into the shared store. Force the
  // user to Cancel first (Cancel flushes/cleans up the batch deterministically).
  wd.setEnabled("buttonClose", !state_.fetch_active);

  // Icon-only Connect / Cert buttons (the .ui clears their text + sets the
  // tooltips). Resolved by the host from its themed icon set; unknown ids fall
  // back to no icon.
  wd.setButtonIconNamed("buttonConnect", "plug_connect");
  wd.setButtonIconNamed("buttonCert", "contract");
  // Refresh uses the host's themed named-icon path (Material "Refresh"), which
  // rasterizes via LoadSvg to a high-res master — crisp on HiDPI, unlike an
  // inline SVG rendered to a logical-size pixmap.
  wd.setButtonIconNamed("buttonRefresh", "refresh");

  // Sequence table — Lua predicate filter + name-substring filter combine
  // to produce the visible-row set. Empty query + empty filter ⇒ all rows
  // visible (clearVisibleRows). The Lua engine is built lazily and reused
  // across getWidgetData calls; per-sequence metadata is injected before
  // each eval and cleared after.
  {
    // Recompute rows + visible only when an input changed (see SeqViewCache);
    // widget_data() runs every host tick, and the per-sequence filter + metadata
    // copies are too heavy to redo at 20Hz on the GUI thread.
    auto& cache = state_.seq_view_cache;
    const bool hit = cache.valid && cache.seq_epoch == state_.seq_epoch && cache.seq_filter == state_.seq_filter &&
                     cache.seq_filter_regex == state_.seq_filter_regex && cache.query_text == state_.query_text &&
                     cache.date_from_ns == state_.date_from_ns && cache.date_to_ns == state_.date_to_ns;
    if (!hit) {
      std::vector<std::vector<std::string>> rows;
      rows.reserve(state_.sequence_names.size());
      for (const auto& rec : state_.sequences) {
        rows.push_back({rec.name, dateOnly(rec.max_ts_ns), formatBytes(rec.total_size_bytes)});
      }

      // Build a schema from the union of every sequence's metadata keys — the
      // PJ3 query engine uses it for shorthand expansion. Only needed when a
      // query is present.
      Schema schema;
      if (!state_.query_text.empty()) {
        for (const auto& rec : state_.sequences) {
          for (const auto& kv : rec.metadata) {
            schema[kv.first].push_back(kv.second);
          }
        }
      }

      // Visible-row set via the shared helper. PJ3 validity-gating: an INVALID
      // metadata query never hides rows (toolbox_mosaico.cpp `if (!valid)
      // return;`); only valid, non-empty queries are evaluated per sequence.
      // Name and date filters always apply regardless of query validity.
      std::vector<FilterSequence> filter_seqs;
      filter_seqs.reserve(state_.sequences.size());
      for (const auto& rec : state_.sequences) {
        filter_seqs.push_back({rec.name, rec.min_ts_ns, rec.max_ts_ns, rec.metadata});
      }
      FilterParams params;
      params.name_filter = state_.seq_filter;
      params.name_regex = state_.seq_filter_regex;
      params.query_text = state_.query_text;
      params.date_from_ns = state_.date_from_ns;
      params.date_to_ns = state_.date_to_ns;

      cache.rows = std::move(rows);
      cache.visible = computeVisibleSequences(filter_seqs, params, schema);
      cache.valid = true;
      cache.seq_epoch = state_.seq_epoch;
      cache.seq_filter = state_.seq_filter;
      cache.seq_filter_regex = state_.seq_filter_regex;
      cache.query_text = state_.query_text;
      cache.date_from_ns = state_.date_from_ns;
      cache.date_to_ns = state_.date_to_ns;
    }

    wd.setTableHeaders("seqTable", {"Name", "Date", "Size"});
    wd.setTableRows("seqTable", cache.rows);
    wd.setVisibleRows("seqTable", cache.visible);
    if (state_.seq_selected_row >= 0) {
      wd.setSelectedRows("seqTable", {state_.seq_selected_row});
    }
    wd.setLabel("seqHeader", fmt::format("Sequences ({}/{})", cache.visible.size(), state_.sequences.size()));
  }

  // Topic table — name-substring filter via visible_rows. Multi-select
  // semantics handled by the .ui (selectionMode=MultiSelection).
  {
    std::vector<std::vector<std::string>> rows;
    std::vector<int> visible;
    rows.reserve(state_.topic_names.size());
    for (size_t i = 0; i < state_.topic_names.size(); ++i) {
      const auto& name = state_.topic_names[i];
      std::string size_text;
      if (i < state_.topic_infos.size()) {
        size_text = formatBytes(state_.topic_infos[i].total_size_bytes);
      }
      rows.push_back({name, size_text});
      if (nameMatches(name, state_.topic_filter, state_.topic_filter_regex)) {
        visible.push_back(static_cast<int>(i));
      }
    }
    wd.setTableHeaders("topicTable", {"Name", "Size"});
    wd.setTableRows("topicTable", rows);
    wd.setVisibleRows("topicTable", visible);
    if (!state_.topic_selected_rows.empty()) {
      wd.setSelectedRows("topicTable", state_.topic_selected_rows);
    }
    if (state_.topics_loading) {
      wd.setLabel("topicHeader", "Topics — loading…");
    } else if (state_.topic_names.empty()) {
      wd.setLabel("topicHeader", "Topics");
    } else {
      wd.setLabel("topicHeader", fmt::format("Topics ({})", state_.topic_names.size()));
    }
  }

  // Info / metadata panel — sequence block on top, then each selected topic.
  // Topic blocks use the full metadata (incl. Arrow schema) cached from
  // getTopicMetadata when available, otherwise the partial listTopics record.
  {
    std::string info_text;
    std::string header = "Info";
    // During a fetch the Info panel doubles as the per-topic progress view
    // (PJ3 DownloadStatsDialog content), since the panel model has no separate
    // progress window. Shows each selected topic's bytes + status.
    if (state_.fetch_active) {
      header = "Download progress";
      info_text += state_.fetch_status + "\n\n";
      for (int row : state_.topic_selected_rows) {
        if (row < 0 || row >= static_cast<int>(state_.topic_names.size())) {
          continue;
        }
        const std::string& tname = state_.topic_names[static_cast<size_t>(row)];
        std::int64_t bytes = 0;
        if (auto it = state_.bytes_by_topic.find(tname); it != state_.bytes_by_topic.end()) {
          bytes = it->second;
        }
        std::string status = "downloading…";
        if (auto it = state_.topic_fetch_status.find(tname);
            it != state_.topic_fetch_status.end() && !it->second.empty()) {
          status = it->second;
        }
        // Per-topic rolling speed over the same 5 s window the aggregate uses,
        // surfacing PJ3 DownloadStatsDialog's per-topic Speed column. Only shown
        // while the topic is still transferring (Done/Failed/Cancelled are static).
        double topic_bps = 0.0;
        if (auto sit = state_.speed_samples.find(tname); sit != state_.speed_samples.end() && sit->second.size() >= 2) {
          const auto& s = sit->second;
          const std::int64_t dt_ms = s.back().ms - s.front().ms;
          if (dt_ms > 0) {
            topic_bps = static_cast<double>(s.back().bytes - s.front().bytes) * 1000.0 / static_cast<double>(dt_ms);
          }
        }
        const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
        const bool transferring = (status == "downloading…" || status == "Cancelling…");
        if (transferring) {
          info_text += fmt::format(
              "{}\n    {:.2f} MiB · {:.2f} MiB/s · {}\n", tname, mib, topic_bps / (1024.0 * 1024.0), status);
        } else {
          info_text += fmt::format("{}\n    {:.2f} MiB · {}\n", tname, mib, status);
        }
      }
      wd.setPlainText("dataView", info_text);
      wd.setLabel("dataViewHeader", header);
    } else {
      const SequenceRecord* seq_rec = nullptr;
      if (!state_.selected_sequence.empty()) {
        for (const auto& s : state_.sequences) {
          if (s.name == state_.selected_sequence) {
            seq_rec = &s;
            break;
          }
        }
      }
      if (seq_rec != nullptr) {
        info_text += buildSequenceInfoText(*seq_rec);
        header = fmt::format("Info — {}", seq_rec->name);
      }
      for (int row : state_.topic_selected_rows) {
        if (row < 0 || row >= static_cast<int>(state_.topic_names.size())) {
          continue;
        }
        const std::string& tname = state_.topic_names[static_cast<size_t>(row)];
        info_text += "\n";
        info_text += std::string(40, '-');
        info_text += "\n\n";
        if (auto it = state_.topic_meta.find(tname); it != state_.topic_meta.end()) {
          info_text += buildTopicInfoText(it->second);
        } else if (row < static_cast<int>(state_.topic_infos.size())) {
          info_text += buildTopicInfoText(state_.topic_infos[static_cast<size_t>(row)]);
          info_text += "  (loading schema…)\n";
        }
      }
      wd.setPlainText("dataView", info_text);
      wd.setLabel("dataViewHeader", header);
    }
  }

  if (state_.open_cert_pending) {
    state_.open_cert_pending = false;
    // Pre-fill the cert sub-dialog with the saved credentials for this server
    // (PJ3 CertDialog parity). PanelEngine applies these to the sub-dialog's
    // certPath / apiKey / allowInsecure inputs before showing it. We surface
    // the *saved* values, not the MOSAICO_API_KEY env
    // fallback, so the dialog reflects what's actually persisted.
    const ServerCredentials saved = loadCredentialsForUri(settings_, state_.uri);
    wd.setText("certPath", saved.cert_path);
    wd.setText("apiKey", saved.api_key);
    wd.setChecked("allowInsecure", saved.allow_insecure);
    // Open the embedded cert_dialog.ui as a read-only modal popup. The
    // existing requestSubDialog mechanism in dialog_protocol only surfaces
    // the UI — there's no roundtrip of user edits back to the plugin yet.
    // PJ3-parity persistence reads cert path + api key from the settings store
    // (loadCredentialsForUri) on next Connect; the Cert dialog surface here
    // gives the user a way to inspect/initiate that flow.
    wd.requestSubDialog(kCertDialogUi);
  }

  if (state_.close_pending) {
    wd.requestClose("user_back");
    state_.close_pending = false;
  }

  return wd.toJson();
}

bool MosaicoDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (widget_name == "comboUri") {
    state_.uri = std::string(text);
    return true;
  }
  if (widget_name == "seqFilter") {
    state_.seq_filter = std::string(text);
    return true;
  }
  if (widget_name == "topicFilter") {
    state_.topic_filter = std::string(text);
    return true;
  }
  if (widget_name == "lua_queryBar") {
    state_.query_text = std::string(text);
    return true;
  }
  // Cert sub-dialog input widgets: panel_engine fires onTextChanged for
  // each text/checkable child after the user clicks OK,
  // followed by an onClicked("subDialogAccepted") to commit. We just
  // stage the value; commit happens in onClicked below.
  if (widget_name == "certPath") {
    state_.pending_cert_path = std::string(text);
    state_.has_pending_cert_edit = true;
    return true;
  }
  if (widget_name == "apiKey") {
    state_.pending_api_key = std::string(text);
    state_.has_pending_api_key_edit = true;
    return true;
  }
  if (widget_name == "allowInsecure") {
    // Checkable values arrive through the typed `checked` channel as
    // booleans (onToggled), not through onTextChanged. The legacy
    // string-encoded "true"/"false" path is kept for cases where the
    // event arrives as text — defensive only.
    state_.pending_allow_insecure = (text == "true");
    state_.has_pending_allow_insecure_edit = true;
    return true;
  }
  return false;
}

bool MosaicoDialog::onClicked(std::string_view widget_name) {
  if (widget_name == "buttonClose") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.close_pending = true;
    return true;
  }
  if (widget_name == "buttonConnect") {
    std::string uri;
    std::string cert_path;
    std::string api_key;
    bool allow_insecure = false;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      uri = state_.uri;
    }
    // Pull saved credentials keyed by the normalized server URI (PJ3 parity:
    // dedupes "grpc+tls://X:6726", "GRPC+TLS://X:6726", and "x:6726" to the
    // same cache entry), with MOSAICO_API_KEY env fallback.
    auto creds = resolveCredentials(settings_, uri);
    cert_path = creds.cert_path;
    api_key = creds.api_key;
    allow_insecure = creds.allow_insecure;

    // Printable-ASCII gate (PJ3 main_window.cpp:947-1013). The host/cert/api-key
    // are headers/URIs handed straight into gRPC, which asserts-and-aborts on
    // control bytes (CR/LF/NUL). Reject here — before the worker is dispatched —
    // and abort the connect so the worst outcome is a visible notification.
    if (!isPrintableAscii(uri)) {
      notify(PJ::ToolboxMessageLevel::kError, "Server URI contains invalid characters (control or non-ASCII bytes).");
      return true;
    }
    if (!isPrintableAscii(cert_path)) {
      notify(PJ::ToolboxMessageLevel::kError, "TLS certificate path contains invalid characters.");
      return true;
    }
    if (!isPrintableAscii(api_key)) {
      notify(PJ::ToolboxMessageLevel::kError, "API key contains invalid characters (control or non-ASCII bytes).");
      return true;
    }

    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.connecting = true;
      state_.suppress_connect_error = false;  // explicit Connect reports failures
    }
    notify(PJ::ToolboxMessageLevel::kInfo, fmt::format("Connecting to {}…", uri));
    postCommand([w = worker_.get(), uri, cert_path, api_key, allow_insecure] {
      w->connectAsync(uri, cert_path, api_key, allow_insecure);
    });
    return true;
  }
  if (widget_name == "buttonRefresh") {
    // Re-list sequences without a disconnect/reconnect (PJ3 onRefreshClicked,
    // main_window.cpp:933-945). No-op unless connected and idle.
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      if (!state_.connected || state_.connecting || state_.fetch_active) {
        return true;
      }
    }
    notify(PJ::ToolboxMessageLevel::kInfo, "Refreshing sequences…");
    postCommand([w = worker_.get()] { w->listSequencesAsync(); });
    return true;
  }
  if (widget_name == "buttonCert") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.open_cert_pending = true;
    return true;
  }
  // Regex-mode toggles (checkable PushButtons): one click = one toggle, so the
  // plugin flips its own flag in lock-step with the button's visual state.
  if (widget_name == "seqRegexToggle") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.seq_filter_regex = !state_.seq_filter_regex;
    return true;
  }
  if (widget_name == "topicRegexToggle") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.topic_filter_regex = !state_.topic_filter_regex;
    return true;
  }
  if (widget_name == "subDialogAccepted") {
    // Cert sub-dialog committed. Merge any staged edits over the
    // currently-cached credentials and write back to the settings store keyed
    // by the current URI. The next buttonConnect click will re-read
    // these via the credential cache and hand them to MosaicoClient.
    std::string uri;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      uri = state_.uri;
    }
    ServerCredentials updated = loadCredentialsForUri(settings_, uri);
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      if (state_.has_pending_cert_edit) {
        updated.cert_path = state_.pending_cert_path;
      }
      if (state_.has_pending_api_key_edit) {
        updated.api_key = state_.pending_api_key;
      }
      if (state_.has_pending_allow_insecure_edit) {
        updated.allow_insecure = state_.pending_allow_insecure;
      }
      state_.has_pending_cert_edit = false;
      state_.has_pending_api_key_edit = false;
      state_.has_pending_allow_insecure_edit = false;
      state_.pending_cert_path.clear();
      state_.pending_api_key.clear();
      state_.pending_allow_insecure = false;
    }
    saveCredentialsForUri(settings_, uri, updated);
    notify(PJ::ToolboxMessageLevel::kInfo, "Credentials saved");
    return true;
  }
  if (widget_name == "buttonFetch") {
    std::string seq;
    std::vector<std::string> topics;
    std::int64_t start = 0;
    std::int64_t end = 0;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      if (!state_.connected || state_.selected_sequence.empty()) {
        notify(PJ::ToolboxMessageLevel::kWarning, "Select a sequence + topic(s) first");
        return true;
      }
      seq = state_.selected_sequence;
      for (int row : state_.topic_selected_rows) {
        if (row >= 0 && row < static_cast<int>(state_.topic_names.size())) {
          topics.push_back(state_.topic_names[row]);
        }
      }
      // Step 7: convert proportional 0..100 % into absolute nanoseconds
      // against the selected sequence's [min_ts, max_ts]. Falls back to
      // unbounded if we don't have time bounds (e.g. metadata stripped).
      const SequenceRecord* rec = nullptr;
      for (const auto& s : state_.sequences) {
        if (s.name == state_.selected_sequence) {
          rec = &s;
          break;
        }
      }
      if (rec != nullptr && rec->max_ts_ns > rec->min_ts_ns) {
        const std::int64_t span = rec->max_ts_ns - rec->min_ts_ns;
        start = rec->min_ts_ns + (span * state_.range_lower) / DialogState::kSliderSteps;
        // The server retrieval window is [start, end) — the upper bound is
        // EXCLUSIVE. At a full-range selection the proportional end lands
        // exactly on max_ts_ns, which would silently drop the final frame, so
        // extend one tick past it when the slider is pinned to 100%.
        end = (state_.range_upper >= DialogState::kSliderSteps)
                  ? rec->max_ts_ns + 1
                  : rec->min_ts_ns + (span * state_.range_upper) / DialogState::kSliderSteps;
      }
      if (topics.empty()) {
        notify(PJ::ToolboxMessageLevel::kWarning, "Select a sequence + topic(s) first");
        return true;
      }
      // Reset the per-batch fetch ledger (PJ3 parity: a batch completes via
      // allFetchesComplete, not per-topic).
      state_.fetch_active = true;
      state_.cancelling = false;
      state_.fetch_total = static_cast<int>(topics.size());
      state_.fetch_done = 0;
      state_.fetch_failed = 0;
      state_.imported_any = false;
      state_.error_counts.clear();
      state_.bytes_by_topic.clear();
      state_.speed_samples.clear();
      state_.topic_fetch_status.clear();
    }
    worker_->resetCancel();
    notify(PJ::ToolboxMessageLevel::kInfo, fmt::format("Fetching {} topic(s)…", topics.size()));
    // Multi-topic parallel pull (Step 10.2). Single topic still goes
    // via this path — the SDK handles the degenerate 1-topic case fine
    // and the per-topic completion signals are uniform.
    postCommand([w = worker_.get(), seq, topics = std::move(topics), start, end]() mutable {
      w->pullTopicsAsync(seq, std::move(topics), start, end);
    });
    persistState();  // crash-resilient: remember query + range at fetch time
    return true;
  }
  if (widget_name == "buttonCancel") {
    worker_->requestCancel();
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.cancelling = true;
      // Reflect the cancel in the Info-panel progress header immediately, and
      // mark every not-yet-finished topic "Cancelling…" so the per-topic view
      // updates without waiting for allFetchesComplete (PJ3 parity).
      state_.fetch_status = "Cancelling…";
      for (int row : state_.topic_selected_rows) {
        if (row < 0 || row >= static_cast<int>(state_.topic_names.size())) {
          continue;
        }
        const std::string& tname = state_.topic_names[static_cast<size_t>(row)];
        auto it = state_.topic_fetch_status.find(tname);
        if (it == state_.topic_fetch_status.end() || it->second.empty() || it->second == "downloading…") {
          state_.topic_fetch_status[tname] = "Cancelling…";
        }
      }
    }
    notify(PJ::ToolboxMessageLevel::kInfo, "Cancelling…");
    return true;
  }
  return false;
}

bool MosaicoDialog::onToggled(std::string_view widget_name, bool checked) {
  if (widget_name == "allowInsecure") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.pending_allow_insecure = checked;
    state_.has_pending_allow_insecure_edit = true;
    return true;
  }
  return false;
}

bool MosaicoDialog::onValueChanged(std::string_view /*widget_name*/, int /*value*/) {
  return false;
}

bool MosaicoDialog::onRangeChanged(std::string_view widget_name, int lower, int upper) {
  if (widget_name == "rangeSlider") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.range_lower = std::clamp(lower, 0, DialogState::kSliderSteps);
    state_.range_upper = std::clamp(upper, 0, DialogState::kSliderSteps);
    return true;
  }
  return false;
}

bool MosaicoDialog::onDateRangeChanged(
    std::string_view widget_name, std::string_view from_iso, std::string_view to_iso) {
  if (widget_name != "datePicker") {
    return false;
  }
  // The DateRangePicker emits UTC ISO datetimes (date + time-of-day, empty =
  // unbounded). Convert to epoch-ns (ms precision) for the interval filter.
  constexpr std::int64_t kNsPerMs = 1'000'000LL;
  auto floorDiv = [](std::int64_t value, std::int64_t divisor) {
    const std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
  };
  auto toEpochNsAtMsPrecision = [&](std::int64_t ns) { return floorDiv(ns, kNsPerMs) * kNsPerMs; };
  const auto from_ns = parseIso8601Utc(from_iso);
  const auto to_ns = parseIso8601Utc(to_iso);
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.date_from_ns = from_ns.has_value() ? toEpochNsAtMsPrecision(*from_ns) : 0;
  state_.date_to_ns = to_ns.has_value() ? toEpochNsAtMsPrecision(*to_ns) : 0;
  return true;
}

void MosaicoDialog::ensureQuerySchemaLocked() {
  if (state_.query_schema_epoch == state_.seq_epoch) {
    return;
  }
  state_.query_schema.clear();
  for (const auto& rec : state_.sequences) {
    for (const auto& kv : rec.metadata) {
      state_.query_schema[kv.first].push_back(kv.second);
    }
  }
  state_.query_schema_epoch = state_.seq_epoch;
}

bool MosaicoDialog::onCodeChangedWithCursor(std::string_view widget_name, std::string_view code, int cursor) {
  if (widget_name != "lua_queryBar") {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.query_text = std::string(code);
  state_.query_cursor = cursor >= 0 ? cursor : static_cast<int>(state_.query_text.size());
  return true;
}

bool MosaicoDialog::onIndexChanged(std::string_view widget_name, int index) {
  if (index < 0) {
    return false;
  }
  enum class Which { kKey, kOp, kVal };
  Which which;
  if (widget_name == "keyCombo") {
    which = Which::kKey;
  } else if (widget_name == "opCombo") {
    which = Which::kOp;
  } else if (widget_name == "valCombo") {
    which = Which::kVal;
  } else {
    return false;
  }

  std::lock_guard<std::mutex> lock(state_.mu);
  ensureQuerySchemaLocked();
  const int cursor = clampQueryCursor(state_.query_cursor, state_.query_text);
  const CursorContext ctx = analyze(state_.query_text, cursor, state_.query_schema);

  // Resolve the picked item and the action for this dropdown. The item lists
  // mirror what widget_data() populated from the same (text, cursor, schema).
  std::string item;
  Action action = Action::Disabled;
  if (which == Which::kKey) {
    const auto keys = schemaKeys(state_.query_schema);
    if (index >= static_cast<int>(keys.size())) {
      return true;
    }
    item = keys[static_cast<std::size_t>(index)];
    action = ctx.key_action;
  } else if (which == Which::kOp) {
    const auto& ops = operators();
    if (index >= static_cast<int>(ops.size())) {
      return true;
    }
    item = ops[static_cast<std::size_t>(index)];
    action = ctx.op_action;
  } else {
    const auto values = schemaValues(state_.query_schema, ctx.context_key);
    if (index >= static_cast<int>(values.size())) {
      return true;
    }
    item = values[static_cast<std::size_t>(index)];
    action = ctx.val_action;
  }
  if (action == Action::Disabled) {
    return true;  // dropdown wasn't actionable at this cursor position
  }

  // A value is a Lua literal: string values must be quoted so they lex as a
  // Value, not a bare Key (keys/operators are inserted verbatim).
  const std::string insert_text = (which == Which::kVal) ? quoteValueForQuery(item) : item;
  const EditResult result = applyCompletion(state_.query_text, ctx, action, insert_text);

  state_.query_text = result.text;
  state_.query_cursor = result.cursor;
  state_.query_push_pending = true;  // push the rewritten text + caret back to the editor
  return true;
}

bool MosaicoDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name == "seqTable") {
    if (selected.empty()) {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.seq_selected_row = -1;
      state_.selected_sequence.clear();
      state_.topic_names.clear();
      state_.topic_selected_rows.clear();
      state_.topics_loading = false;
      return true;
    }
    std::string seq = selected.front();
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.selected_sequence = selected.front();
      // A manual pick supersedes any pending restore: drop the staged sequence/
      // topic restore so the next topic list isn't re-selected against stale
      // persisted names.
      state_.restore_selected_sequence.clear();
      state_.restore_selected_topics.clear();
      // Map the name back to a row index for the visible-row highlight.
      for (size_t i = 0; i < state_.sequence_names.size(); ++i) {
        if (state_.sequence_names[i] == selected.front()) {
          state_.seq_selected_row = static_cast<int>(i);
          break;
        }
      }
      state_.topic_names.clear();
      state_.topic_selected_rows.clear();
      state_.topics_loading = true;  // header shows "loading…" until topicsReady
    }
    postCommand([w = worker_.get(), seq] { w->listTopicsAsync(seq); });
    return true;
  }
  if (widget_name == "topicTable") {
    std::string seq;
    std::vector<std::string> need_metadata;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.topic_selected_rows.clear();
      seq = state_.selected_sequence;
      for (const auto& sel : selected) {
        for (size_t i = 0; i < state_.topic_names.size(); ++i) {
          if (state_.topic_names[i] == sel) {
            state_.topic_selected_rows.push_back(static_cast<int>(i));
            // Kick off a one-shot metadata fetch (Arrow schema + tag) for the
            // Info panel if we don't already have the full record cached.
            if (state_.topic_meta.find(sel) == state_.topic_meta.end()) {
              need_metadata.push_back(sel);
            }
            break;
          }
        }
      }
    }
    for (const std::string& topic : need_metadata) {
      postCommand([w = worker_.get(), seq, topic] { w->fetchTopicMetadataAsync(seq, topic); });
    }
    return true;
  }
  return false;
}

void MosaicoDialog::onConnectFinished(bool ok, std::string status, std::string error) {
  std::string uri;
  bool plaintext_retry_needed = false;
  std::string plaintext_uri;
  bool suppress_error = false;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.connected = ok;
    uri = state_.uri;

    // PJ3 plaintext-fallback: TLS connect failed, user allowed insecure
    // fallback, no custom cert in use, and we haven't tried plaintext
    // yet for this URI. Switch grpc+tls:// → grpc:// and retry once.
    if (!ok) {
      auto creds = loadCredentialsForUri(settings_, uri);
      const bool no_custom_cert = creds.cert_path.empty();
      if (creds.allow_insecure && no_custom_cert && !state_.attempted_plaintext_fallback) {
        plaintext_uri = uri;
        if (const auto pos = plaintext_uri.find("grpc+tls://"); pos != std::string::npos) {
          plaintext_uri.replace(pos, std::string("grpc+tls://").size(), "grpc://");
        }
        if (plaintext_uri != uri) {
          plaintext_retry_needed = true;
          state_.attempted_plaintext_fallback = true;
        }
      }
    } else {
      state_.attempted_plaintext_fallback = false;
    }
    // Stay "connecting" only while a plaintext retry is about to fire.
    state_.connecting = plaintext_retry_needed;
    suppress_error = state_.suppress_connect_error;
  }

  if (ok) {
    // PJ3 parity: promote a successful URI to the head of the MRU history
    // (cap 20) and persist via the settings store ("mosaico/server_history").
    SettingsStore settings(settings_);
    std::vector<std::string> history = settings.getStringList("mosaico/server_history");
    history = promoteToHead(history, uri, /*cap=*/20);
    settings.setStringList("mosaico/server_history", history);

    notify(PJ::ToolboxMessageLevel::kInfo, status);
    postCommand([w = worker_.get()] { w->listSequencesAsync(); });
    return;
  }

  if (plaintext_retry_needed) {
    postCommand([w = worker_.get(), plaintext_uri] { w->connectAsync(plaintext_uri, {}, {}, true); });
  } else if (!suppress_error) {
    // PJ3 AutoConnect context shows no popup; explicit connects do.
    notify(PJ::ToolboxMessageLevel::kError, fmt::format("Mosaico connection failed: {}", error));
  }
}

void MosaicoDialog::persistState() {
  std::string query;
  int lower = 0;
  int upper = DialogState::kSliderSteps;
  std::string selected_sequence;
  std::vector<std::string> selected_topics;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    query = state_.query_text;
    lower = state_.range_lower;
    upper = state_.range_upper;
    // PJ3 parity: remember the last selection so the next connect can re-select
    // it. Resolve the selected topic ROW indices back to names (names are
    // stable across re-fetch; row indices are not).
    selected_sequence = state_.selected_sequence;
    for (int row : state_.topic_selected_rows) {
      if (row >= 0 && row < static_cast<int>(state_.topic_names.size())) {
        selected_topics.push_back(state_.topic_names[static_cast<std::size_t>(row)]);
      }
    }
  }
  SettingsStore settings(settings_);
  settings.setString("mosaico/metadata_query", query);
  settings.setInt("mosaico/range_lower", lower);
  settings.setInt("mosaico/range_upper", upper);
  settings.setString("mosaico/selected_sequence", selected_sequence);
  settings.setStringList("mosaico/selected_topics", selected_topics);
}

void MosaicoDialog::notify(PJ::ToolboxMessageLevel level, const std::string& message) {
  if (!runtime_host_provider_) {
    return;
  }
  auto runtime = runtime_host_provider_();
  if (runtime.valid()) {
    runtime.reportMessage(level, message);
  }
}

void MosaicoDialog::populateSequencesLocked(std::vector<SequenceInfo>& seqs, bool seed_dates) {
  state_.sequences.clear();
  state_.sequence_names.clear();
  state_.sequences.reserve(seqs.size());
  state_.sequence_names.reserve(seqs.size());
  for (auto& s : seqs) {
    SequenceRecord rec;
    rec.name = s.name;
    rec.min_ts_ns = s.min_ts_ns;
    rec.max_ts_ns = s.max_ts_ns;
    rec.total_size_bytes = s.total_size_bytes;
    // Mosaico SDK's user_metadata is std::unordered_map<string, string>;
    // convert into the std::map<string, string, std::less<>> used by the
    // ported PJ3 Lua engine.
    for (const auto& kv : s.user_metadata) {
      rec.metadata.emplace(kv.first, kv.second);
    }
    state_.sequence_names.push_back(rec.name);
    state_.sequences.push_back(std::move(rec));
  }

  // Compute the global [min, max] timestamp span and (final result only) seed
  // the date-range picker with it (PJ3 shows the data's full range, e.g.
  // 29/04/2016 → 08/04/2020). "All" filter ⇒ the span passes every sequence.
  std::int64_t gmin = 0;
  std::int64_t gmax = 0;
  for (const auto& s : state_.sequences) {
    if (s.min_ts_ns > 0 && (gmin == 0 || s.min_ts_ns < gmin)) {
      gmin = s.min_ts_ns;
    }
    if (s.max_ts_ns > gmax) {
      gmax = s.max_ts_ns;
    }
  }
  state_.global_min_ts_ns = gmin;
  state_.global_max_ts_ns = gmax;
  if (seed_dates && gmin > 0 && gmax > 0) {
    state_.date_from_ns = gmin;
    state_.date_to_ns = gmax;
    state_.date_from_iso = isoFromNs(gmin);
    state_.date_to_iso = isoFromNs(gmax);
  }
  ++state_.seq_epoch;  // invalidate the seqTable view cache
}

void MosaicoDialog::sortSequencesLocked() {
  if (state_.seq_sort_col < 0 || state_.sequences.empty()) {
    return;
  }
  const int col = state_.seq_sort_col;
  const bool asc = state_.seq_sort_asc;
  // Build the column's comparable keys, get a stable permutation, apply it.
  std::vector<std::size_t> perm;
  if (col == 1) {  // Date — numeric on max_ts_ns (the displayed date)
    std::vector<std::int64_t> keys;
    keys.reserve(state_.sequences.size());
    for (const auto& s : state_.sequences) {
      keys.push_back(s.max_ts_ns);
    }
    perm = sortedPermutation(keys, asc);
  } else if (col == 2) {  // Size — numeric on total_size_bytes
    std::vector<std::int64_t> keys;
    keys.reserve(state_.sequences.size());
    for (const auto& s : state_.sequences) {
      keys.push_back(s.total_size_bytes);
    }
    perm = sortedPermutation(keys, asc);
  } else {  // Name (col 0)
    std::vector<std::string> keys;
    keys.reserve(state_.sequences.size());
    for (const auto& s : state_.sequences) {
      keys.push_back(s.name);
    }
    perm = sortedPermutation(keys, asc);
  }

  std::vector<SequenceRecord> reordered;
  reordered.reserve(state_.sequences.size());
  for (std::size_t p : perm) {
    reordered.push_back(std::move(state_.sequences[p]));
  }
  state_.sequences = std::move(reordered);

  state_.sequence_names.clear();
  state_.sequence_names.reserve(state_.sequences.size());
  for (const auto& s : state_.sequences) {
    state_.sequence_names.push_back(s.name);
  }
  // Re-map the selected row to the selected sequence's new position.
  state_.seq_selected_row = -1;
  if (!state_.selected_sequence.empty()) {
    for (std::size_t i = 0; i < state_.sequence_names.size(); ++i) {
      if (state_.sequence_names[i] == state_.selected_sequence) {
        state_.seq_selected_row = static_cast<int>(i);
        break;
      }
    }
  }
  ++state_.seq_epoch;  // row order changed → invalidate the seqTable view cache
}

void MosaicoDialog::sortTopicsLocked() {
  if (state_.topic_sort_col < 0 || state_.topic_names.empty()) {
    return;
  }
  const int col = state_.topic_sort_col;
  const bool asc = state_.topic_sort_asc;
  const bool have_infos = state_.topic_infos.size() == state_.topic_names.size();

  // Capture the selected topics by name so selection survives the reorder.
  std::set<std::string> selected;
  for (int r : state_.topic_selected_rows) {
    if (r >= 0 && r < static_cast<int>(state_.topic_names.size())) {
      selected.insert(state_.topic_names[static_cast<std::size_t>(r)]);
    }
  }

  // Sort an index permutation, then apply it to the parallel name/info vectors.
  std::vector<std::size_t> perm;
  if (col == 1 && have_infos) {  // Size — numeric
    std::vector<std::int64_t> keys;
    keys.reserve(state_.topic_infos.size());
    for (const auto& t : state_.topic_infos) {
      keys.push_back(t.total_size_bytes);
    }
    perm = sortedPermutation(keys, asc);
  } else {  // Name (col 0) or fallback when sizes are unavailable
    perm = sortedPermutation(state_.topic_names, asc);
  }

  std::vector<std::string> new_names;
  new_names.reserve(state_.topic_names.size());
  std::vector<TopicInfo> new_infos;
  if (have_infos) {
    new_infos.reserve(state_.topic_infos.size());
  }
  for (std::size_t p : perm) {
    new_names.push_back(state_.topic_names[p]);
    if (have_infos) {
      new_infos.push_back(state_.topic_infos[p]);
    }
  }
  state_.topic_names = std::move(new_names);
  if (have_infos) {
    state_.topic_infos = std::move(new_infos);
  }

  // Re-map index-based selection from the captured names.
  state_.topic_selected_rows.clear();
  for (std::size_t i = 0; i < state_.topic_names.size(); ++i) {
    if (selected.count(state_.topic_names[i]) > 0) {
      state_.topic_selected_rows.push_back(static_cast<int>(i));
    }
  }
}

std::vector<std::string> MosaicoDialog::restoreSelectedTopicsLocked() {
  std::vector<std::string> need_metadata;
  if (state_.restore_selected_topics.empty()) {
    return need_metadata;
  }
  // One-shot: take the staged names and clear the slot so a later re-fetch or
  // manual selection isn't overridden.
  const std::vector<std::string> wanted = std::move(state_.restore_selected_topics);
  state_.restore_selected_topics.clear();

  state_.topic_selected_rows.clear();
  for (std::size_t i = 0; i < state_.topic_names.size(); ++i) {
    for (const std::string& name : wanted) {
      if (state_.topic_names[i] == name) {
        state_.topic_selected_rows.push_back(static_cast<int>(i));
        // Fetch full metadata (Arrow schema + tag) for the Info panel, same as
        // the manual topic-selection path, when not already cached.
        if (state_.topic_meta.find(name) == state_.topic_meta.end()) {
          need_metadata.push_back(name);
        }
        break;
      }
    }
  }
  return need_metadata;
}

bool MosaicoDialog::onHeaderClicked(std::string_view widget_name, int section) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (widget_name == "seqTable") {
    if (state_.seq_sort_col == section) {
      state_.seq_sort_asc = !state_.seq_sort_asc;
    } else {
      state_.seq_sort_col = section;
      state_.seq_sort_asc = true;
    }
    sortSequencesLocked();
    return true;
  }
  if (widget_name == "topicTable") {
    if (state_.topic_sort_col == section) {
      state_.topic_sort_asc = !state_.topic_sort_asc;
    } else {
      state_.topic_sort_col = section;
      state_.topic_sort_asc = true;
    }
    sortTopicsLocked();
    return true;
  }
  return false;
}

void MosaicoDialog::onSequenceListStarted(std::vector<SequenceInfo> seqs) {
  std::lock_guard<std::mutex> lock(state_.mu);
  // Early populate so the table shows up immediately; leave the date picker
  // untouched (the final sequencesReady seeds it from the complete span).
  populateSequencesLocked(seqs, /*seed_dates=*/false);
  sortSequencesLocked();
}

void MosaicoDialog::onSequenceInfoReady(SequenceInfo seq) {
  std::lock_guard<std::mutex> lock(state_.mu);
  // Fill in this one sequence's detail in place (min/max ts → Date, size →
  // Size, metadata → query schema) so the columns populate incrementally as
  // the server streams detail, instead of snapping in all at once at the final
  // sequencesReady (PJ3 parity). Keyed by name; positions are left as-is
  // (re-sorting per row would make the list jump during load — the final
  // sequencesReady re-sorts once).
  for (auto& rec : state_.sequences) {
    if (rec.name != seq.name) {
      continue;
    }
    rec.min_ts_ns = seq.min_ts_ns;
    rec.max_ts_ns = seq.max_ts_ns;
    rec.total_size_bytes = seq.total_size_bytes;
    rec.metadata.clear();
    for (const auto& kv : seq.user_metadata) {
      rec.metadata.emplace(kv.first, kv.second);
    }
    ++state_.seq_epoch;  // this row's Date/Size changed → invalidate the view cache so it streams in live
    break;
  }
}

void MosaicoDialog::onSequencesReady(std::vector<SequenceInfo> seqs) {
  std::size_t count = 0;
  std::string reselect_sequence;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    populateSequencesLocked(seqs, /*seed_dates=*/true);
    sortSequencesLocked();
    count = state_.sequences.size();

    // PJ3 parity: re-select the persisted sequence if it's present in the
    // freshly-listed sequences and the user hasn't already picked one this
    // session. One-shot — clear the restore slot once consumed so a later
    // manual selection / refresh doesn't snap back.
    if (!state_.restore_selected_sequence.empty() && state_.selected_sequence.empty()) {
      for (std::size_t i = 0; i < state_.sequence_names.size(); ++i) {
        if (state_.sequence_names[i] == state_.restore_selected_sequence) {
          state_.selected_sequence = state_.restore_selected_sequence;
          state_.seq_selected_row = static_cast<int>(i);
          state_.topics_loading = true;  // header shows "loading…" until topicsReady
          reselect_sequence = state_.selected_sequence;
          break;
        }
      }
      state_.restore_selected_sequence.clear();
      if (reselect_sequence.empty()) {
        // The saved sequence is gone from the server — drop any staged topic
        // restore too, since topics are scoped to a sequence.
        state_.restore_selected_topics.clear();
      }
    }
  }
  // Surface the "data arrived" outcome in the app's notification bell.
  notify(PJ::ToolboxMessageLevel::kInfo, fmt::format("{} sequences", count));
  // Kick off the topic list for the re-selected sequence (mirrors the manual
  // onSelectionChanged path); restored topic names are re-applied in onTopicsReady.
  if (!reselect_sequence.empty()) {
    postCommand([w = worker_.get(), reselect_sequence] { w->listTopicsAsync(reselect_sequence); });
  }
}

void MosaicoDialog::onTopicsReady(std::string sequence_name, std::vector<std::string> topic_names) {
  std::vector<std::string> need_metadata;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (sequence_name != state_.selected_sequence) {
      return;  // user moved on
    }
    state_.topic_names = std::move(topic_names);
    state_.topics_loading = false;
    sortTopicsLocked();
    need_metadata = restoreSelectedTopicsLocked();
  }
  for (const std::string& topic : need_metadata) {
    postCommand([w = worker_.get(), sequence_name, topic] { w->fetchTopicMetadataAsync(sequence_name, topic); });
  }
}

void MosaicoDialog::onTopicInfosReady(std::string sequence_name, std::vector<TopicInfo> topics) {
  std::vector<std::string> need_metadata;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (sequence_name != state_.selected_sequence) {
      return;  // user moved on
    }
    state_.topic_infos = std::move(topics);
    // Keep topic_names aligned with topic_infos so size/selection indexing in
    // getWidgetData stays consistent regardless of which signal arrived first.
    state_.topic_names.clear();
    state_.topic_names.reserve(state_.topic_infos.size());
    for (const auto& t : state_.topic_infos) {
      state_.topic_names.push_back(t.topic_name);
    }
    sortTopicsLocked();
    state_.topic_meta.clear();  // schema cache is per-sequence
    need_metadata = restoreSelectedTopicsLocked();
  }
  for (const std::string& topic : need_metadata) {
    postCommand([w = worker_.get(), sequence_name, topic] { w->fetchTopicMetadataAsync(sequence_name, topic); });
  }
}

void MosaicoDialog::onTopicMetadataReady(std::string sequence_name, std::string topic_name, TopicInfo info) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (sequence_name != state_.selected_sequence) {
    return;  // user moved on
  }
  state_.topic_meta[std::move(topic_name)] = std::move(info);
}

void MosaicoDialog::onPullProgress(std::string topic_name, std::int64_t bytes) {
  std::lock_guard<std::mutex> lock(state_.mu);
  // Per-topic byte ledger + rolling 5 s speed sample (PJ3 DownloadStatsDialog
  // parity). "done" is the count of topics that have fully completed
  // (pullFinished), not those merely streaming.
  state_.bytes_by_topic[topic_name] = bytes;
  const std::int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  auto& samples = state_.speed_samples[topic_name];
  samples.push_back({now_ms, bytes});
  // Trim to the trailing 5 s window.
  const std::int64_t window_start = now_ms - 5000;
  while (samples.size() > 2 && samples.front().ms < window_start) {
    samples.erase(samples.begin());
  }
  std::int64_t total_bytes = 0;
  for (const auto& kv : state_.bytes_by_topic) {
    total_bytes += kv.second;
  }
  // Aggregate download rate from the trailing-window samples (sum of per-topic
  // rates) — PJ3 DownloadStatsDialog surfaced this; here it rides the status
  // line since the panel has no separate progress window.
  double bytes_per_sec = 0.0;
  for (const auto& [topic, samples] : state_.speed_samples) {
    if (samples.size() >= 2) {
      const std::int64_t dt_ms = samples.back().ms - samples.front().ms;
      if (dt_ms > 0) {
        bytes_per_sec +=
            static_cast<double>(samples.back().bytes - samples.front().bytes) * 1000.0 / static_cast<double>(dt_ms);
      }
    }
  }
  const double mib = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
  const double mibps = bytes_per_sec / (1024.0 * 1024.0);
  // High-frequency progress: shown in the Info panel during the fetch, NOT
  // pushed to the notification bell (it would flood the diagnostics log).
  // Once the user has hit Cancel, in-flight topics emit a few more progress
  // ticks before observing the flag — don't let them overwrite the
  // "Cancelling…" header set in onClicked(buttonCancel).
  if (!state_.cancelling) {
    state_.fetch_status = fmt::format(
        "Fetching: {}/{} topics, {:.2f} MiB ({:.2f} MiB/s)", state_.fetch_done, state_.fetch_total, mib, mibps);
  }
}

void MosaicoDialog::onPullFinished(std::string /*sequence_name*/, std::string topic_name, bool ok, std::string error) {
  std::lock_guard<std::mutex> lock(state_.mu);
  // PJ3 parity: tally per-topic results into the batch ledger. The panel does
  // NOT close here — that happens once in onAllFetchesComplete after the whole
  // batch lands. Closing on the first topic (the old behaviour) tore down the
  // worker mid-stream and dropped every remaining topic.
  ++state_.fetch_done;
  if (ok) {
    state_.imported_any = true;
    state_.topic_fetch_status[topic_name] = "Done";
  } else if (state_.cancelling) {
    // Interrupted by the user's Cancel, not a real failure: label it
    // "Cancelled" and keep it OUT of the error tally so a cancel doesn't
    // raise spurious "Mosaico fetch errors" notifications.
    state_.topic_fetch_status[topic_name] = "Cancelled";
  } else {
    ++state_.fetch_failed;
    state_.topic_fetch_status[topic_name] = "Failed";
    // Collapse identical messages so "[3x] no data" reads once, not thrice.
    ++state_.error_counts[std::move(error)];
  }
}

void MosaicoDialog::onAllFetchesComplete(std::string sequence_name) {
  FetchSummary summary;
  int total = 0;
  int failed = 0;
  bool was_cancelled = false;
  bool imported_any = false;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.fetch_active = false;
    total = state_.fetch_total;
    failed = state_.fetch_failed;
    was_cancelled = state_.cancelling;
    imported_any = state_.imported_any;
    summary = buildFetchSummary(
        state_.fetch_total, state_.fetch_done, state_.fetch_failed, state_.imported_any, state_.cancelling,
        state_.error_counts);
    state_.cancelling = false;
    if (summary.should_close) {
      state_.close_pending = true;  // PJ3 parity: panel closes after a successful batch.
    }
  }

  if (!summary.error_summary.empty()) {
    notify(PJ::ToolboxMessageLevel::kError, fmt::format("Mosaico fetch errors:\n{}", summary.error_summary));
  }

  // Flush buffered writer chunks into the engine and rebuild the catalog once
  // for the whole batch. appendArrowStream/pushOwnedObject only buffer data into
  // the shared DataWriter/ObjectStore — without notifyDataChanged the imported
  // topics never appear in the dataset tree.
  //
  // CANCEL SEMANTICS — DELIBERATE DIVERGENCE FROM PJ3 (gap #10):
  // PJ3 DISCARDS all partial data on cancel (toolbox_mosaico.cpp:227-230): it
  // could do so freely because PJ3 stages every batch in a LOCAL
  // PlotDataMapRef (imported_data_) and only emits importData() into the live
  // store at onAllFetchesComplete — so a cancel just drops the unimported
  // buffer.
  // PJ4 has no such staging buffer: each topic's data is written DIRECTLY into
  // the shared host DataWriter / ObjectStore in pullTopicsAsync's on_done as it
  // completes (the [C1] streaming-write design). By the time Cancel is observed,
  // topics that already finished have ALREADY written into the live store, and
  // the ToolboxHostView ABI exposes no removeDataSource / removeTopic to roll
  // those writes back. So we KEEP the topics that finished before the cancel and
  // flush them here — surfacing real, already-landed data is strictly safer than
  // a fake "discard" that would leave the catalog/store inconsistent. The user
  // is told exactly what happened via the "kept the topics that finished first"
  // notification below.
  if (imported_any && runtime_host_provider_) {
    auto runtime = runtime_host_provider_();
    if (runtime.valid()) {
      runtime.notifyDataChanged();
    }
  }

  if (was_cancelled) {
    notify(
        PJ::ToolboxMessageLevel::kInfo,
        imported_any ? "Download cancelled — kept the topics that finished first" : "Download cancelled");
  } else if (summary.should_import) {
    // imported = completed topics. Valid only on the clean-success path: here
    // `failed` is real failures and there were no cancellations (cancelled
    // topics deliberately don't increment fetch_failed), so total - failed is
    // exactly the imported count.
    const int imported_count = total - failed;
    notify(
        PJ::ToolboxMessageLevel::kInfo,
        fmt::format("Imported {}/{} topics from {}", imported_count, total, sequence_name));
  }
}

}  // namespace mosaico
