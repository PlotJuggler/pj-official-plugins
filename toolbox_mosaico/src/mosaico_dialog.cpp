// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

// clang-format off
// Arrow headers MUST precede any Qt include (mosaico_dialog.hpp pulls in
// QObject): arrow/util/cancel.h collides with Qt's 'signals' macro otherwise.
// Order is load-bearing — keep clang-format from sorting the main header up.
#include <arrow/api.h>

#include "mosaico_dialog.hpp"
// clang-format on

#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

#include "cert_dialog_ui.hpp"
#include "credentials_store.hpp"
#include "date_filter.h"
#include "fetch_summary.h"
#include "fetch_worker.hpp"
#include "mosaico_panel_manifest.hpp"
#include "mosaico_panel_ui.hpp"
#include "name_filter.h"
#include "query/engine.h"
#include "query/query.h"
#include "query_filter.h"
#include "server_history.h"
#include "table_sort.h"

namespace mosaico {

namespace {

// Load per-server credentials, falling back to the MOSAICO_API_KEY environment
// variable when no key is cached for this server (PJ3 automation parity).
ServerCredentials resolveCredentials(const QString& uri) {
  ServerCredentials creds = loadCredentials(uri);
  if (creds.api_key.isEmpty()) {
    if (const char* env = std::getenv("MOSAICO_API_KEY"); env != nullptr && env[0] != '\0') {
      creds.api_key = QString::fromUtf8(env);
    }
  }
  return creds;
}

// Read an embedded Qt-resource SVG into a byte string. The app registers
// resources.qrc process-wide, so the in-process plugin can pull shared icons
// (e.g. the material glyphs) straight from it. Returns "" if the path is absent.
std::string readQrcSvg(const char* path) {
  QFile f(QString::fromLatin1(path));
  if (!f.open(QIODevice::ReadOnly)) {
    return {};
  }
  const QByteArray bytes = f.readAll();
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// ---------------------------------------------------------------------------
// Info-panel / table formatting helpers (ported from PJ3 data_view_panel.cpp
// and format_utils.h). The Info panel is rendered as monospaced plain text.
// ---------------------------------------------------------------------------

// Human-readable byte counts, matching PJ3 formatBytes(): 1 decimal for
// KB/MB/GB, integer for bytes, empty for non-positive.
std::string formatBytes(std::int64_t bytes) {
  if (bytes <= 0) {
    return {};
  }
  char buf[32];
  const double b = static_cast<double>(bytes);
  if (bytes >= 1'000'000'000LL) {
    std::snprintf(buf, sizeof(buf), "%.1f GB", b / 1'000'000'000.0);
  } else if (bytes >= 1'000'000LL) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", b / 1'000'000.0);
  } else if (bytes >= 1'000LL) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1'000.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
  }
  return std::string(buf);
}

std::string isoFromNs(std::int64_t ts_ns) {
  if (ts_ns <= 0) {
    return {};
  }
  auto dt = QDateTime::fromMSecsSinceEpoch(ts_ns / 1'000'000LL, QTimeZone::utc());
  return dt.toString(Qt::ISODate).toStdString();
}

QString dateOnly(std::int64_t ts_ns) {
  if (ts_ns <= 0) {
    return QStringLiteral("--/--/----");
  }
  auto dt = QDateTime::fromMSecsSinceEpoch(ts_ns / 1'000'000LL, QTimeZone::utc());
  return dt.toString(QStringLiteral("dd/MM/yyyy"));
}

QString dateTimeUtc(std::int64_t ts_ns) {
  auto dt = QDateTime::fromMSecsSinceEpoch(ts_ns / 1'000'000LL, QTimeZone::utc());
  return dt.toString(QStringLiteral("dd/MM/yyyy HH:mm:ss 'UTC'"));
}

template <typename MapType>
QString formatMetadata(const MapType& metadata, const QString& indent = QString()) {
  if (metadata.empty()) {
    return {};
  }
  // Deterministic ordering — unordered_map iteration order is unspecified.
  std::vector<std::pair<std::string, std::string>> sorted(metadata.begin(), metadata.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  QString text;
  for (const auto& [key, value] : sorted) {
    text += QString("%1%2:\n%1  %3\n").arg(indent, QString::fromStdString(key), QString::fromStdString(value));
  }
  return text;
}

// Recursively format an Arrow field with vertical indentation for structs and
// lists, exactly like PJ3 data_view_panel::formatFieldType.
void formatFieldType(const std::shared_ptr<arrow::Field>& field, const QString& indent, QString& out) {
  auto type = field->type();
  if (type->id() == arrow::Type::STRUCT) {
    out += QString("%1%2\n").arg(indent, QString::fromStdString(field->name()));
    auto st = std::static_pointer_cast<arrow::StructType>(type);
    for (int i = 0; i < st->num_fields(); ++i) {
      formatFieldType(st->field(i), indent + "  ", out);
    }
  } else if (type->id() == arrow::Type::LIST) {
    auto inner = std::static_pointer_cast<arrow::ListType>(type)->value_field();
    out += QString("%1%2 []\n").arg(indent, QString::fromStdString(field->name()));
    if (inner->type()->id() == arrow::Type::STRUCT) {
      auto st = std::static_pointer_cast<arrow::StructType>(inner->type());
      for (int i = 0; i < st->num_fields(); ++i) {
        formatFieldType(st->field(i), indent + "  ", out);
      }
    } else {
      out += QString("%1  %2\n").arg(indent, QString::fromStdString(inner->type()->ToString()));
    }
  } else {
    out += QString("%1%2 : %3\n")
               .arg(indent, QString::fromStdString(field->name()), QString::fromStdString(type->ToString()));
  }
}

QString formatSchemaFields(const std::shared_ptr<arrow::Schema>& schema) {
  QString text;
  if (!schema) {
    return text;
  }
  text += QString("Fields (%1):\n").arg(schema->num_fields());
  for (int i = 0; i < schema->num_fields(); ++i) {
    formatFieldType(schema->field(i), "  ", text);
  }
  return text;
}

QString buildSequenceInfoText(const SequenceRecord& rec) {
  QString text;
  text += QString("Sequence : %1\n").arg(QString::fromStdString(rec.name));
  if (rec.max_ts_ns > 0) {
    text += QString("Date     : %1\n").arg(dateOnly(rec.max_ts_ns));
  }
  if (rec.total_size_bytes > 0) {
    text += QString("Size     : %1\n").arg(QString::fromStdString(formatBytes(rec.total_size_bytes)));
  }
  if (!rec.metadata.empty()) {
    text += "\nMetadata:\n";
    text += formatMetadata(rec.metadata);
  }
  return text;
}

QString buildTopicInfoText(const TopicInfo& info) {
  QString text;
  text += QString("Topic    : %1\n").arg(QString::fromStdString(info.topic_name));
  if (!info.ontology_tag.empty()) {
    text += QString("Tag      : %1\n").arg(QString::fromStdString(info.ontology_tag));
  }
  if (info.created_at_ns > 0) {
    text += QString("Created  : %1\n").arg(dateTimeUtc(info.created_at_ns));
  }
  if (info.locked) {
    if (info.completed_at_ns.has_value() && *info.completed_at_ns > 0) {
      text += QString("Status   : sealed (%1)\n").arg(dateTimeUtc(*info.completed_at_ns));
    } else {
      text += "Status   : sealed\n";
    }
  } else {
    text += "Status   : live\n";
  }
  if (info.chunks_number > 0) {
    text += QString("Chunks   : %1\n").arg(info.chunks_number);
  }
  if (info.total_size_bytes > 0) {
    text += QString("Size     : %1\n").arg(QString::fromStdString(formatBytes(info.total_size_bytes)));
  }
  if (!info.resource_locator.empty()) {
    text += QString("Resource : %1\n").arg(QString::fromStdString(info.resource_locator));
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

MosaicoDialog::MosaicoDialog() : worker_(new FetchWorker()) {
  worker_->moveToThread(&worker_thread_);
  QObject::connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);

  // SequenceInfo / TopicInfo are registered as Qt metatypes so they can cross
  // thread boundaries via queued connections.
  qRegisterMetaType<std::vector<SequenceInfo>>("std::vector<SequenceInfo>");
  qRegisterMetaType<std::vector<TopicInfo>>("std::vector<TopicInfo>");
  qRegisterMetaType<TopicInfo>("TopicInfo");
  qRegisterMetaType<SequenceInfo>("SequenceInfo");

  QObject::connect(worker_, &FetchWorker::connectFinished, this, &MosaicoDialog::onConnectFinished);
  QObject::connect(worker_, &FetchWorker::sequencesReady, this, &MosaicoDialog::onSequencesReady);
  QObject::connect(worker_, &FetchWorker::sequenceListStarted, this, &MosaicoDialog::onSequenceListStarted);
  QObject::connect(worker_, &FetchWorker::sequenceInfoReady, this, &MosaicoDialog::onSequenceInfoReady);
  QObject::connect(worker_, &FetchWorker::topicsReady, this, &MosaicoDialog::onTopicsReady);
  QObject::connect(worker_, &FetchWorker::topicInfosReady, this, &MosaicoDialog::onTopicInfosReady);
  QObject::connect(worker_, &FetchWorker::topicMetadataReady, this, &MosaicoDialog::onTopicMetadataReady);
  QObject::connect(worker_, &FetchWorker::pullProgress, this, &MosaicoDialog::onPullProgress);
  QObject::connect(worker_, &FetchWorker::pullFinished, this, &MosaicoDialog::onPullFinished);
  QObject::connect(worker_, &FetchWorker::allFetchesComplete, this, &MosaicoDialog::onAllFetchesComplete);

  worker_thread_.start();

  // Restore persisted UI state and auto-connect to the last server (PJ3
  // parity). Safe to touch QSettings + state_ unlocked here — single-threaded
  // at construction, before the tick loop or any worker result can run.
  QStringList history = QSettings().value(QStringLiteral("mosaico/server_history")).toStringList();
  if (!history.isEmpty()) {
    state_.uri = history.first().toStdString();
  }
  state_.query_text = QSettings().value(QStringLiteral("mosaico/metadata_query")).toString().toStdString();
  state_.range_lower =
      std::clamp(QSettings().value(QStringLiteral("mosaico/range_lower"), 0).toInt(), 0, DialogState::kSliderSteps);
  state_.range_upper = std::clamp(
      QSettings().value(QStringLiteral("mosaico/range_upper"), DialogState::kSliderSteps).toInt(), 0,
      DialogState::kSliderSteps);

  if (!history.isEmpty()) {
    const QString uri = QString::fromStdString(state_.uri);
    const ServerCredentials creds = resolveCredentials(uri);
    state_.connecting = true;
    state_.suppress_connect_error = true;  // PJ3 AutoConnect: no error notification on failure.
    QMetaObject::invokeMethod(
        worker_, "connectAsync", Qt::QueuedConnection, Q_ARG(QString, uri), Q_ARG(QString, creds.cert_path),
        Q_ARG(QString, creds.api_key), Q_ARG(bool, creds.allow_insecure));
  }
}

MosaicoDialog::~MosaicoDialog() {
  // Persist the Lua query + slider proportions for next open (PJ3 parity).
  persistState();
  // Signal the SDK's mid-stream cancel flag before quitting the event
  // loop — otherwise wait() blocks indefinitely while a pullTopics is
  // in flight on the worker thread.
  if (worker_ != nullptr) {
    worker_->requestCancel();
  }
  worker_thread_.quit();
  worker_thread_.wait();
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
    QStringList history = QSettings().value(QStringLiteral("mosaico/server_history")).toStringList();
    bool has_demo = false;
    for (const auto& h : history) {
      const std::string s = h.toStdString();
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

  // MetadataQueryBar: a self-contained, verbatim copy of PJ3's QueryBar. The
  // WIDGET owns the editor text, the context-aware key/op/value combos, and the
  // inline completion — all driven from the schema it's fed below. The plugin's
  // only jobs here are: feed the full schema, push the persisted query text ONCE
  // to restore it, and push Lua validity feedback (lua lives in the plugin).
  {
    // One-shot text restore. Pushing query_text every tick would clobber the
    // widget's own edits the instant the user types or picks from a dropdown
    // (the stale-echo race that broke the previous split design). After the
    // first push the widget owns the text; edits return via onCodeChanged.
    if (!state_.query_text_pushed) {
      wd.setCodeContent("lua_queryBar", state_.query_text);
      state_.query_text_pushed = true;
    }

    // Full schema: union of metadata key→values across all sequences. nlohmann
    // serializes the map as {key: [values]}; the host rebuilds the engine Schema
    // and populates the key combo + completion from it.
    std::map<std::string, std::vector<std::string>> schema;
    for (const auto& rec : state_.sequences) {
      for (const auto& kv : rec.metadata) {
        auto& vals = schema[kv.first];
        if (std::find(vals.begin(), vals.end(), kv.second) == vals.end()) {
          vals.push_back(kv.second);
        }
      }
    }
    wd.setQuerySchema("lua_queryBar", schema);

    // Validation feedback via the Lua engine (matches PJ3 QueryBar::onTextChanged,
    // except PJ3 computed it inside the widget — here the widget delegates to the
    // plugin because lua/sol2 stays plugin-side).
    if (state_.query_text.empty()) {
      wd.setQueryFeedback("lua_queryBar", "", true);
    } else {
      const bool valid = Engine::validate(state_.query_text).valid;
      wd.setQueryFeedback("lua_queryBar", valid ? "ok" : "invalid syntax", valid);
    }
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

  // SequencePicker: hint the dataset's earliest date as the "from" placeholder.
  if (state_.global_min_ts_ns > 0) {
    const QDate d = QDateTime::fromMSecsSinceEpoch(state_.global_min_ts_ns / 1'000'000LL, QTimeZone::utc()).date();
    wd.setDatePickerEarliest("datePicker", d.toString(Qt::ISODate).toStdString());
  }

  // Button enable/disable (PJ3 parity): Connect is inert while a connect or a
  // fetch is in flight; Download needs a sequence + topic(s) and an idle
  // worker; Cancel is live only during a fetch.
  wd.setEnabled("buttonConnect", !state_.connecting && !state_.fetch_active);
  wd.setEnabled(
      "buttonFetch", state_.connected && !state_.selected_sequence.empty() && !state_.topic_selected_rows.empty() &&
                         !state_.fetch_active);
  wd.setEnabled("buttonCancel", state_.fetch_active);
  // Closing mid-fetch tears the worker down before allFetchesComplete runs,
  // stranding the topics that already wrote into the shared store. Force the
  // user to Cancel first (Cancel flushes/cleans up the batch deterministically).
  wd.setEnabled("buttonClose", !state_.fetch_active);

  // Icon-only Connect / Cert buttons (the .ui clears their text + sets the
  // tooltips). The glyphs are the app's shared material icons pulled from the
  // process-wide qrc; the host tints them to the button text color. Loaded once.
  static const std::string kConnectIcon = readQrcSvg(":/resources/svg/plug_connect.svg");
  static const std::string kCertIcon = readQrcSvg(":/resources/svg/contract.svg");
  if (!kConnectIcon.empty()) {
    wd.setButtonIcon("buttonConnect", kConnectIcon);
  }
  if (!kCertIcon.empty()) {
    wd.setButtonIcon("buttonCert", kCertIcon);
  }

  // Sequence table — Lua predicate filter + name-substring filter combine
  // to produce the visible-row set. Empty query + empty filter ⇒ all rows
  // visible (clearVisibleRows). The Lua engine is built lazily and reused
  // across getWidgetData calls; per-sequence metadata is injected before
  // each eval and cleared after.
  {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(state_.sequence_names.size());
    for (const auto& rec : state_.sequences) {
      rows.push_back({rec.name, dateOnly(rec.max_ts_ns).toStdString(), formatBytes(rec.total_size_bytes)});
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
    params.date_every_day = state_.date_every_day;
    params.date_from_tod_ms = state_.date_from_tod_ms;
    params.date_to_tod_ms = state_.date_to_tod_ms;
    const std::vector<int> visible = computeVisibleSequences(filter_seqs, params, schema);

    wd.setTableHeaders("seqTable", {"Name", "Date", "Size"});
    wd.setTableRows("seqTable", rows);
    wd.setVisibleRows("seqTable", visible);
    if (state_.seq_selected_row >= 0) {
      wd.setSelectedRows("seqTable", {state_.seq_selected_row});
    }
    wd.setLabel(
        "seqHeader",
        QStringLiteral("Sequences (%1/%2)").arg(visible.size()).arg(state_.sequences.size()).toStdString());
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
      wd.setLabel("topicHeader", QStringLiteral("Topics (%1)").arg(state_.topic_names.size()).toStdString());
    }
  }

  // Info / metadata panel — sequence block on top, then each selected topic.
  // Topic blocks use the full metadata (incl. Arrow schema) cached from
  // getTopicMetadata when available, otherwise the partial listTopics record.
  {
    QString info_text;
    QString header = QStringLiteral("Info");
    // During a fetch the Info panel doubles as the per-topic progress view
    // (PJ3 DownloadStatsDialog content), since the panel model has no separate
    // progress window. Shows each selected topic's bytes + status.
    if (state_.fetch_active) {
      header = QStringLiteral("Download progress");
      info_text += QString::fromStdString(state_.fetch_status) + "\n\n";
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
          info_text += QStringLiteral("%1\n    %2 MiB · %3 MiB/s · %4\n")
                           .arg(QString::fromStdString(tname))
                           .arg(mib, 0, 'f', 2)
                           .arg(topic_bps / (1024.0 * 1024.0), 0, 'f', 2)
                           .arg(QString::fromStdString(status));
        } else {
          info_text += QStringLiteral("%1\n    %2 MiB · %3\n")
                           .arg(QString::fromStdString(tname))
                           .arg(mib, 0, 'f', 2)
                           .arg(QString::fromStdString(status));
        }
      }
      wd.setPlainText("dataView", info_text.toStdString());
      wd.setLabel("dataViewHeader", header.toStdString());
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
        header = QStringLiteral("Info — %1").arg(QString::fromStdString(seq_rec->name));
      }
      for (int row : state_.topic_selected_rows) {
        if (row < 0 || row >= static_cast<int>(state_.topic_names.size())) {
          continue;
        }
        const std::string& tname = state_.topic_names[static_cast<size_t>(row)];
        info_text += "\n" + QString(40, QChar('-')) + "\n\n";
        if (auto it = state_.topic_meta.find(tname); it != state_.topic_meta.end()) {
          info_text += buildTopicInfoText(it->second);
        } else if (row < static_cast<int>(state_.topic_infos.size())) {
          info_text += buildTopicInfoText(state_.topic_infos[static_cast<size_t>(row)]);
          info_text += QStringLiteral("  (loading schema…)\n");
        }
      }
      wd.setPlainText("dataView", info_text.toStdString());
      wd.setLabel("dataViewHeader", header.toStdString());
    }
  }

  if (state_.open_cert_pending) {
    state_.open_cert_pending = false;
    // Pre-fill the cert sub-dialog with the saved credentials for this server
    // (PJ3 CertDialog parity). PanelEngine applies these to the sub-dialog's
    // certPath / apiKey / allowInsecure inputs before showing it. We surface
    // the *saved* values (loadCredentials), not the MOSAICO_API_KEY env
    // fallback, so the dialog reflects what's actually persisted.
    const ServerCredentials saved = loadCredentials(QString::fromStdString(state_.uri));
    wd.setText("certPath", saved.cert_path.toStdString());
    wd.setText("apiKey", saved.api_key.toStdString());
    wd.setChecked("allowInsecure", saved.allow_insecure);
    // Open the embedded cert_dialog.ui as a read-only modal popup. The
    // existing requestSubDialog mechanism in dialog_protocol only surfaces
    // the UI — there's no roundtrip of user edits back to the plugin yet.
    // PJ3-parity persistence (Step 10.4) reads cert path + api key from
    // QSettings via credentials_store on next Connect; the Cert dialog
    // surface here gives the user a way to inspect/initiate that flow.
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
  // Cert sub-dialog input widgets: panel_engine fires onTextChanged
  // for each QLineEdit/QCheckBox child after the user clicks OK,
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
    // QCheckBox values arrive through the typed `checked` channel as
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
    QString uri;
    QString cert_path;
    QString api_key;
    bool allow_insecure = false;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.connecting = true;
      state_.suppress_connect_error = false;  // explicit Connect reports failures
      uri = QString::fromStdString(state_.uri);
    }
    notify(PJ::ToolboxMessageLevel::kInfo, QStringLiteral("Connecting to %1…").arg(uri).toStdString());
    // Pull saved credentials keyed by the normalized server URI (PJ3 parity:
    // dedupes "grpc+tls://X:6726", "GRPC+TLS://X:6726", and "x:6726" to the
    // same cache entry), with MOSAICO_API_KEY env fallback.
    auto creds = resolveCredentials(uri);
    cert_path = creds.cert_path;
    api_key = creds.api_key;
    allow_insecure = creds.allow_insecure;
    QMetaObject::invokeMethod(
        worker_, "connectAsync", Qt::QueuedConnection, Q_ARG(QString, uri), Q_ARG(QString, cert_path),
        Q_ARG(QString, api_key), Q_ARG(bool, allow_insecure));
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
    // currently-cached credentials and write back to QSettings keyed
    // by the current URI. The next buttonConnect click will re-read
    // these via loadCredentials() and hand them to MosaicoClient.
    QString uri;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      uri = QString::fromStdString(state_.uri);
    }
    ServerCredentials updated = loadCredentials(uri);
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      if (state_.has_pending_cert_edit) {
        updated.cert_path = QString::fromStdString(state_.pending_cert_path);
      }
      if (state_.has_pending_api_key_edit) {
        updated.api_key = QString::fromStdString(state_.pending_api_key);
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
    saveCredentials(uri, updated);
    notify(PJ::ToolboxMessageLevel::kInfo, "Credentials saved");
    return true;
  }
  if (widget_name == "buttonFetch") {
    QString seq;
    QStringList topics;
    qint64 start = 0;
    qint64 end = 0;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      if (!state_.connected || state_.selected_sequence.empty()) {
        notify(PJ::ToolboxMessageLevel::kWarning, "Select a sequence + topic(s) first");
        return true;
      }
      seq = QString::fromStdString(state_.selected_sequence);
      for (int row : state_.topic_selected_rows) {
        if (row >= 0 && row < static_cast<int>(state_.topic_names.size())) {
          topics.append(QString::fromStdString(state_.topic_names[row]));
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
        const qint64 span = rec->max_ts_ns - rec->min_ts_ns;
        start = rec->min_ts_ns + (span * state_.range_lower) / DialogState::kSliderSteps;
        // The server retrieval window is [start, end) — the upper bound is
        // EXCLUSIVE. At a full-range selection the proportional end lands
        // exactly on max_ts_ns, which would silently drop the final frame, so
        // extend one tick past it when the slider is pinned to 100%.
        end = (state_.range_upper >= DialogState::kSliderSteps)
                  ? rec->max_ts_ns + 1
                  : rec->min_ts_ns + (span * state_.range_upper) / DialogState::kSliderSteps;
      }
      if (topics.isEmpty()) {
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
    notify(PJ::ToolboxMessageLevel::kInfo, QStringLiteral("Fetching %1 topic(s)…").arg(topics.size()).toStdString());
    // Multi-topic parallel pull (Step 10.2). Single topic still goes
    // via this path — the SDK handles the degenerate 1-topic case fine
    // and the per-topic completion signals are uniform.
    QMetaObject::invokeMethod(
        worker_, "pullTopicsAsync", Qt::QueuedConnection, Q_ARG(QString, seq), Q_ARG(QStringList, topics),
        Q_ARG(qint64, start), Q_ARG(qint64, end));
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
    std::string_view widget_name, std::string_view from_iso, std::string_view to_iso, bool every_day) {
  if (widget_name != "datePicker") {
    return false;
  }
  // The SequencePicker emits UTC ISO datetimes (date + time-of-day, empty =
  // unbounded). Convert to epoch-ns for the interval filter, and also keep the
  // time-of-day component for the recurring "Every day" window.
  auto parse = [](std::string_view iso) -> QDateTime {
    if (iso.empty()) {
      return {};
    }
    return QDateTime::fromString(QString::fromUtf8(iso.data(), static_cast<int>(iso.size())), Qt::ISODate);
  };
  const QDateTime from_dt = parse(from_iso);
  const QDateTime to_dt = parse(to_iso);
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.date_from_ns = from_dt.isValid() ? static_cast<qint64>(from_dt.toMSecsSinceEpoch()) * 1'000'000LL : 0;
  state_.date_to_ns = to_dt.isValid() ? static_cast<qint64>(to_dt.toMSecsSinceEpoch()) * 1'000'000LL : 0;
  state_.date_every_day = every_day;
  if (from_dt.isValid()) {
    state_.date_from_tod_ms = from_dt.time().msecsSinceStartOfDay();
  }
  if (to_dt.isValid()) {
    state_.date_to_tod_ms = to_dt.time().msecsSinceStartOfDay();
  }
  return true;
}

bool MosaicoDialog::onCodeChanged(std::string_view widget_name, std::string_view code) {
  if (widget_name != "lua_queryBar") {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.query_text = std::string(code);
  return true;
}

bool MosaicoDialog::onQuerySelector(
    std::string_view /*widget_name*/, std::string_view /*role*/, std::string_view /*value*/) {
  // Dead path: the self-contained MetadataQueryBar now owns its combos and edits
  // the editor locally, so it no longer emits a separate selector event — the
  // edited text arrives via onCodeChanged. Kept as a harmless no-op because it's
  // a DialogPlugin base-class override.
  return false;
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
    QString seq = QString::fromStdString(selected.front());
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.selected_sequence = selected.front();
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
    QMetaObject::invokeMethod(worker_, "listTopicsAsync", Qt::QueuedConnection, Q_ARG(QString, seq));
    return true;
  }
  if (widget_name == "topicTable") {
    QString seq;
    std::vector<QString> need_metadata;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.topic_selected_rows.clear();
      seq = QString::fromStdString(state_.selected_sequence);
      for (const auto& sel : selected) {
        for (size_t i = 0; i < state_.topic_names.size(); ++i) {
          if (state_.topic_names[i] == sel) {
            state_.topic_selected_rows.push_back(static_cast<int>(i));
            // Kick off a one-shot metadata fetch (Arrow schema + tag) for the
            // Info panel if we don't already have the full record cached.
            if (state_.topic_meta.find(sel) == state_.topic_meta.end()) {
              need_metadata.push_back(QString::fromStdString(sel));
            }
            break;
          }
        }
      }
    }
    for (const QString& topic : need_metadata) {
      QMetaObject::invokeMethod(
          worker_, "fetchTopicMetadataAsync", Qt::QueuedConnection, Q_ARG(QString, seq), Q_ARG(QString, topic));
    }
    return true;
  }
  return false;
}

void MosaicoDialog::onConnectFinished(bool ok, QString status, QString error) {
  QString uri;
  bool plaintext_retry_needed = false;
  QString plaintext_uri;
  bool suppress_error = false;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.connected = ok;
    uri = QString::fromStdString(state_.uri);

    // PJ3 plaintext-fallback: TLS connect failed, user allowed insecure
    // fallback, no custom cert in use, and we haven't tried plaintext
    // yet for this URI. Switch grpc+tls:// → grpc:// and retry once.
    if (!ok) {
      auto creds = loadCredentials(uri);
      const bool no_custom_cert = creds.cert_path.isEmpty();
      if (creds.allow_insecure && no_custom_cert && !state_.attempted_plaintext_fallback) {
        plaintext_uri = uri;
        plaintext_uri.replace(QStringLiteral("grpc+tls://"), QStringLiteral("grpc://"));
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
    // (cap 20) and persist via QSettings ("mosaico/server_history").
    QStringList history = QSettings().value(QStringLiteral("mosaico/server_history")).toStringList();
    history = promoteToHead(history, uri, /*cap=*/20);
    QSettings().setValue(QStringLiteral("mosaico/server_history"), history);

    notify(PJ::ToolboxMessageLevel::kInfo, status.toStdString());
    QMetaObject::invokeMethod(worker_, "listSequencesAsync", Qt::QueuedConnection);
    return;
  }

  if (plaintext_retry_needed) {
    QMetaObject::invokeMethod(
        worker_, "connectAsync", Qt::QueuedConnection, Q_ARG(QString, plaintext_uri), Q_ARG(QString, QString()),
        Q_ARG(QString, QString()), Q_ARG(bool, true));
  } else if (!suppress_error) {
    // PJ3 AutoConnect context shows no popup; explicit connects do.
    notify(PJ::ToolboxMessageLevel::kError, QStringLiteral("Mosaico connection failed: %1").arg(error).toStdString());
  }
}

void MosaicoDialog::persistState() {
  std::string query;
  int lower = 0;
  int upper = DialogState::kSliderSteps;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    query = state_.query_text;
    lower = state_.range_lower;
    upper = state_.range_upper;
  }
  QSettings settings;
  settings.setValue(QStringLiteral("mosaico/metadata_query"), QString::fromStdString(query));
  settings.setValue(QStringLiteral("mosaico/range_lower"), lower);
  settings.setValue(QStringLiteral("mosaico/range_upper"), upper);
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
    break;
  }
}

void MosaicoDialog::onSequencesReady(std::vector<SequenceInfo> seqs) {
  std::size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    populateSequencesLocked(seqs, /*seed_dates=*/true);
    sortSequencesLocked();
    count = state_.sequences.size();
  }
  // Surface the "data arrived" outcome in the app's notification bell.
  notify(PJ::ToolboxMessageLevel::kInfo, QStringLiteral("%1 sequences").arg(count).toStdString());
}

void MosaicoDialog::onTopicsReady(QString sequence_name, QStringList topic_names) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (sequence_name.toStdString() != state_.selected_sequence) {
    return;  // user moved on
  }
  state_.topic_names.clear();
  state_.topic_names.reserve(static_cast<size_t>(topic_names.size()));
  for (const auto& n : topic_names) {
    state_.topic_names.push_back(n.toStdString());
  }
  state_.topics_loading = false;
  sortTopicsLocked();
}

void MosaicoDialog::onTopicInfosReady(QString sequence_name, std::vector<TopicInfo> topics) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (sequence_name.toStdString() != state_.selected_sequence) {
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
}

void MosaicoDialog::onTopicMetadataReady(QString sequence_name, QString topic_name, TopicInfo info) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (sequence_name.toStdString() != state_.selected_sequence) {
    return;  // user moved on
  }
  state_.topic_meta[topic_name.toStdString()] = std::move(info);
}

void MosaicoDialog::onPullProgress(QString topic_name, qint64 bytes) {
  std::lock_guard<std::mutex> lock(state_.mu);
  // Per-topic byte ledger + rolling 5 s speed sample (PJ3 DownloadStatsDialog
  // parity). "done" is the count of topics that have fully completed
  // (pullFinished), not those merely streaming.
  const std::string key = topic_name.toStdString();
  state_.bytes_by_topic[key] = bytes;
  const std::int64_t now_ms = QDateTime::currentMSecsSinceEpoch();
  auto& samples = state_.speed_samples[key];
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
    state_.fetch_status = QStringLiteral("Fetching: %1/%2 topics, %3 MiB (%4 MiB/s)")
                              .arg(state_.fetch_done)
                              .arg(state_.fetch_total)
                              .arg(mib, 0, 'f', 2)
                              .arg(mibps, 0, 'f', 2)
                              .toStdString();
  }
}

void MosaicoDialog::onPullFinished(QString /*sequence_name*/, QString topic_name, bool ok, QString error) {
  std::lock_guard<std::mutex> lock(state_.mu);
  // PJ3 parity: tally per-topic results into the batch ledger. The panel does
  // NOT close here — that happens once in onAllFetchesComplete after the whole
  // batch lands. Closing on the first topic (the old behaviour) tore down the
  // worker mid-stream and dropped every remaining topic.
  ++state_.fetch_done;
  if (ok) {
    state_.imported_any = true;
    state_.topic_fetch_status[topic_name.toStdString()] = "Done";
  } else if (state_.cancelling) {
    // Interrupted by the user's Cancel, not a real failure: label it
    // "Cancelled" and keep it OUT of the error tally so a cancel doesn't
    // raise spurious "Mosaico fetch errors" notifications.
    state_.topic_fetch_status[topic_name.toStdString()] = "Cancelled";
  } else {
    ++state_.fetch_failed;
    state_.topic_fetch_status[topic_name.toStdString()] = "Failed";
    // Collapse identical messages so "[3x] no data" reads once, not thrice.
    ++state_.error_counts[error.toStdString()];
  }
}

void MosaicoDialog::onAllFetchesComplete(QString sequence_name) {
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
    notify(
        PJ::ToolboxMessageLevel::kError,
        QStringLiteral("Mosaico fetch errors:\n%1").arg(QString::fromStdString(summary.error_summary)).toStdString());
  }

  // Flush buffered writer chunks into the engine and rebuild the catalog once
  // for the whole batch. appendArrowStream/pushOwnedObject only QUEUE data into
  // the shared DataWriter/ObjectStore — without notifyDataChanged the imported
  // topics never appear in the dataset tree. Crucially this runs even on cancel:
  // topics that finished BEFORE the user cancelled have already written, so we
  // must surface them (PJ3 refreshed regardless of cancel) instead of leaving
  // them stranded/invisible in the writer until some later fetch.
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
        PJ::ToolboxMessageLevel::kInfo, QStringLiteral("Imported %1/%2 topics from %3")
                                            .arg(imported_count)
                                            .arg(total)
                                            .arg(sequence_name)
                                            .toStdString());
  }
}

}  // namespace mosaico
