// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// anomaly_runner — headless CLI for the Anomaly Detection pipeline (Task F).
//
// Loads a data file through a PlotJuggler DataSource plugin (no GUI, no
// pj_datastore: a hand-rolled in-memory capture write-host receives the decoded
// series), runs the SAME Lua detection rule the GUI editor uses (anomaly_core),
// and emits a structured JSON report (pass/fail + anomalies + severities). The
// process exit code is 0 on pass and 1 on fail, so it drops straight into CI /
// data pipelines.
//
//   anomaly_runner --data run.csv --script "Spike (point)" --source anomaly_demo/value
//   anomaly_runner --data run.csv --script my_rule.lua --source topic/field --out report.json
//   anomaly_runner --list-functions
//
// Phase 3a handles DIRECT_INGEST sources (CSV); MCAP (DELEGATED_INGEST) is the
// next step (it additionally wires a MessageParser plugin into push_message).

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_base/sdk/testing/parser_write_recorder.hpp>
#include <pj_base/span.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/message_parser_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "anomaly_core.hpp"
#include "notify.hpp"

namespace {

std::string viewToString(PJ_string_view_t sv) {
  return std::string(sv.data == nullptr ? "" : sv.data, sv.size);
}

// Coerce any numeric scalar to double; nullopt for strings / unspecified.
std::optional<double> scalarToDouble(const PJ_scalar_value_t& v) {
  switch (v.type) {
    case PJ_PRIMITIVE_TYPE_FLOAT32:
      return static_cast<double>(v.data.as_float32);
    case PJ_PRIMITIVE_TYPE_FLOAT64:
      return v.data.as_float64;
    case PJ_PRIMITIVE_TYPE_INT8:
      return static_cast<double>(v.data.as_int8);
    case PJ_PRIMITIVE_TYPE_INT16:
      return static_cast<double>(v.data.as_int16);
    case PJ_PRIMITIVE_TYPE_INT32:
      return static_cast<double>(v.data.as_int32);
    case PJ_PRIMITIVE_TYPE_INT64:
      return static_cast<double>(v.data.as_int64);
    case PJ_PRIMITIVE_TYPE_UINT8:
      return static_cast<double>(v.data.as_uint8);
    case PJ_PRIMITIVE_TYPE_UINT16:
      return static_cast<double>(v.data.as_uint16);
    case PJ_PRIMITIVE_TYPE_UINT32:
      return static_cast<double>(v.data.as_uint32);
    case PJ_PRIMITIVE_TYPE_UINT64:
      return static_cast<double>(v.data.as_uint64);
    case PJ_PRIMITIVE_TYPE_BOOL:
      return v.data.as_bool ? 1.0 : 0.0;
    default:
      return std::nullopt;  // string / unspecified -> not a plottable scalar
  }
}

// ---------------------------------------------------------------------------
// CaptureHost — an in-memory DataSource write-host. It implements the C-ABI the
// datasource plugin pushes into, buffering each (topic/field) into an
// anomaly_core::SeriesAccessor. SDK-only: no pj_datastore.
// ---------------------------------------------------------------------------

class CaptureHost {
 public:
  std::map<std::string, anomaly_core::SeriesAccessor> series;  // "topic/field" -> samples
  std::vector<std::string> order;                              // first-seen order of names

  PJ_source_write_host_t writeHost() {
    static const PJ_source_write_host_vtable_t vtable = {
        .abi_version = PJ_PLUGIN_DATA_API_VERSION,
        .struct_size = sizeof(PJ_source_write_host_vtable_t),
        .ensure_topic = &CaptureHost::ensureTopic,
        .ensure_field = &CaptureHost::ensureField,
        .append_record = &CaptureHost::appendRecord,
        .append_bound_record = &CaptureHost::appendBoundRecord,
        .append_arrow_stream = &CaptureHost::appendArrowStream,
    };
    return PJ_source_write_host_t{.ctx = this, .vtable = &vtable};
  }

  PJ_data_source_runtime_host_t runtimeHost() {
    static const PJ_data_source_runtime_host_vtable_t vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_data_source_runtime_host_vtable_t),
        .report_message = &CaptureHost::reportMessage,
        .progress_start = &CaptureHost::progressStart,
        .progress_update = &CaptureHost::progressUpdate,
        .progress_finish = &CaptureHost::progressFinish,
        .is_stop_requested = &CaptureHost::isStopRequested,
        .notify_state = &CaptureHost::notifyState,
        .request_stop = &CaptureHost::requestStop,
        .ensure_parser_binding = &CaptureHost::ensureParserBinding,
        .show_message_box = &CaptureHost::showMessageBox,
        .list_available_encodings = &CaptureHost::listEncodings,
        .push_message = &CaptureHost::pushMessage,
        .notify_available_topics = &CaptureHost::notifyAvailableTopics,
    };
    return PJ_data_source_runtime_host_t{.ctx = this, .vtable = &vtable};
  }

  /// Directory to resolve MessageParser plugin .so files from (delegated ingest).
  void setPluginDir(std::filesystem::path dir) {
    plugin_dir_ = std::move(dir);
  }

 private:
  std::map<std::uint32_t, std::string> topic_names_;
  std::uint32_t next_topic_id_ = 1;

  // --- Delegated ingest (MCAP): parser registry ---
  struct Binding {
    std::unique_ptr<PJ::MessageParserHandle> handle;
    std::unique_ptr<PJ::sdk::testing::ParserWriteRecorder> recorder;
    std::string topic;
    std::map<std::string, anomaly_core::SeriesAccessor*> cols;  // field name -> series slot (cached)
  };
  std::filesystem::path plugin_dir_;
  std::map<std::string, std::unique_ptr<PJ::MessageParserLibrary>> parser_libs_;  // .so -> lib
  std::map<std::uint32_t, Binding> bindings_;
  std::uint32_t next_binding_id_ = 1;

  static std::string parserSoForEncoding(const std::string& enc) {
    if (enc == "cdr" || enc == "ros2msg" || enc == "ros1msg" || enc == "ros1" || enc == "ros2") {
      return "libparser_ros_plugin.so";
    }
    if (enc == "protobuf") {
      return "libparser_protobuf_plugin.so";
    }
    if (enc == "json") {
      return "libparser_json_plugin.so";
    }
    return "";
  }

  // Load (cached) the parser for an encoding, create a handle bound to a fresh
  // recorder, bind the schema/config, and remember it under a new binding id.
  bool doEnsureParserBinding(const PJ_parser_binding_request_t* req, PJ_parser_binding_handle_t* out, PJ_error_t*) {
    const std::string encoding = viewToString(req->parser_encoding);
    const std::string so = parserSoForEncoding(encoding);
    if (so.empty()) {
      return false;  // unsupported encoding -> the source skips this channel
    }
    auto lib_it = parser_libs_.find(so);
    if (lib_it == parser_libs_.end()) {
      auto lib = PJ::MessageParserLibrary::load((plugin_dir_ / so).string());
      if (!lib) {
        return false;
      }
      lib_it = parser_libs_.emplace(so, std::make_unique<PJ::MessageParserLibrary>(std::move(*lib))).first;
    }

    Binding b;
    b.topic = viewToString(req->topic_name);
    b.recorder = std::make_unique<PJ::sdk::testing::ParserWriteRecorder>();
    b.handle = std::make_unique<PJ::MessageParserHandle>(lib_it->second->createHandle());

    PJ::ServiceRegistryBuilder reg;
    reg.registerService<PJ::sdk::ParserWriteHostService>(b.recorder->makeHost());
    if (auto st = b.handle->bind(reg.view()); !st) {
      return false;
    }
    const std::string cfg = viewToString(req->parser_config_json);
    if (!cfg.empty()) {
      (void)b.handle->loadConfig(cfg);
    }
    if (req->schema.size > 0 && req->schema.data != nullptr) {
      (void)b.handle->bindSchema(
          viewToString(req->type_name), PJ::Span<const uint8_t>(req->schema.data, req->schema.size));
    }
    const std::uint32_t id = next_binding_id_++;
    bindings_.emplace(id, std::move(b));
    *out = PJ_parser_binding_handle_t{id};
    return true;
  }

  // Fetch the message bytes, parse them into the binding's recorder, and drain the
  // decoded numeric fields into our (topic/field) series buffers.
  bool doPushMessage(PJ_parser_binding_handle_t binding, int64_t ts, PJ_message_data_fetcher_t fetch) {
    auto it = bindings_.find(binding.id);
    if (it == bindings_.end()) {
      if (fetch.release != nullptr) {
        fetch.release(fetch.ctx);
      }
      return false;
    }
    Binding& b = it->second;
    PJ_payload_t payload{};
    PJ_error_t ferr{};
    const bool got = fetch.fetchMessageData(fetch.ctx, &payload, &ferr);
    if (!got) {
      if (fetch.release != nullptr) {
        fetch.release(fetch.ctx);
      }
      return false;
    }
    b.recorder->rows().clear();
    // Forward the real per-message host timestamp (MCAP log-time, etc.). Parsers
    // with their own embedded stamp override it; stampless parsers (json/protobuf)
    // use it verbatim, so hardcoding 0 here collapsed every headless sample to
    // t=0 and diverged from the GUI (which forwards the same timestamp).
    const auto st = b.handle->parse(PJ::Timestamp{ts}, PJ::Span<const uint8_t>(payload.data, payload.size));
    if (payload.anchor.release != nullptr) {
      payload.anchor.release(payload.anchor.ctx);
    }
    if (fetch.release != nullptr) {
      fetch.release(fetch.ctx);
    }
    if (!st) {
      return true;  // tolerate a single bad message
    }
    for (const auto& row : b.recorder->rows()) {
      for (const auto& f : row.fields) {
        if (f.is_null || f.type == PJ::PrimitiveType::kString) {
          continue;
        }
        const double v = (f.type == PJ::PrimitiveType::kBool) ? (f.bool_value ? 1.0 : 0.0) : f.numeric;
        // Resolve the "topic/field" series slot once per field (the series map is
        // node-based, so the cached pointer stays valid as new series are added).
        auto cit = b.cols.find(f.name);
        if (cit == b.cols.end()) {
          cit = b.cols.emplace(f.name, &slot(PJ::sdk::markerSeriesKey(b.topic, f.name))).first;
        }
        cit->second->timestamps.push_back(static_cast<double>(row.timestamp));
        cit->second->values.push_back(v);
      }
    }
    return true;
  }

  anomaly_core::SeriesAccessor& slot(const std::string& name) {
    auto [it, inserted] = series.try_emplace(name);
    if (inserted) {
      order.push_back(name);
    }
    return it->second;
  }

  // --- source write-host vtable ---
  static bool ensureTopic(void* ctx, PJ_string_view_t name, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
    auto* self = static_cast<CaptureHost*>(ctx);
    const std::uint32_t id = self->next_topic_id_++;
    self->topic_names_[id] = viewToString(name);
    *out = PJ_topic_handle_t{id};
    return true;
  }

  static bool ensureField(
      void*, PJ_topic_handle_t topic, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out,
      PJ_error_t*) noexcept {
    *out = PJ_field_handle_t{topic, 0};  // we resolve by name in append_record
    return true;
  }

  static bool appendRecord(
      void* ctx, PJ_topic_handle_t topic, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t field_count,
      PJ_error_t*) noexcept {
    auto* self = static_cast<CaptureHost*>(ctx);
    auto tit = self->topic_names_.find(topic.id);
    const std::string topic_name = (tit == self->topic_names_.end()) ? "" : tit->second;
    for (uint64_t i = 0; i < field_count; ++i) {
      const PJ_named_field_value_t& f = fields[i];
      if (f.is_null) {
        continue;
      }
      const std::optional<double> v = scalarToDouble(f.value);
      if (!v) {
        continue;  // skip strings
      }
      const std::string key = PJ::sdk::markerSeriesKey(topic_name, viewToString(f.name));
      anomaly_core::SeriesAccessor& sa = self->slot(key);
      sa.timestamps.push_back(static_cast<double>(timestamp));
      sa.values.push_back(*v);
    }
    return true;
  }

  static bool appendBoundRecord(
      void*, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, uint64_t, PJ_error_t*) noexcept {
    return true;  // CSV uses the named path; not needed here
  }

  static bool appendArrowStream(
      void*, PJ_topic_handle_t, struct ArrowArrayStream* stream, PJ_string_view_t, PJ_error_t*) noexcept {
    if (stream != nullptr && stream->release != nullptr) {
      stream->release(stream);  // honour the ownership contract
    }
    return true;
  }

  // --- runtime host vtable (minimal stubs) ---
  static void reportMessage(void*, PJ_data_source_message_level_t, PJ_string_view_t) noexcept {}
  static bool progressStart(void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept {
    return true;
  }
  static bool progressUpdate(void*, uint64_t) noexcept {
    return true;
  }
  static void progressFinish(void*) noexcept {}
  static bool isStopRequested(void*) noexcept {
    return false;
  }
  static void notifyState(void*, PJ_data_source_state_t) noexcept {}
  static void requestStop(void*, PJ_data_source_state_t, PJ_string_view_t) noexcept {}
  static bool ensureParserBinding(
      void* ctx, const PJ_parser_binding_request_t* req, PJ_parser_binding_handle_t* out, PJ_error_t* err) noexcept {
    return static_cast<CaptureHost*>(ctx)->doEnsureParserBinding(req, out, err);
  }
  static int showMessageBox(void*, PJ_message_box_type_t, PJ_string_view_t, PJ_string_view_t, int) noexcept {
    return PJ_MSG_BTN_OK;
  }
  static const char* listEncodings(void*) noexcept {
    return "[]";
  }
  static bool pushMessage(
      void* ctx, PJ_parser_binding_handle_t binding, int64_t ts, PJ_message_data_fetcher_t fetch,
      PJ_error_t*) noexcept {
    return static_cast<CaptureHost*>(ctx)->doPushMessage(binding, ts, fetch);
  }
  // Advertised-topic streaming is a live-source concern; file ingest accepts and ignores it.
  static bool notifyAvailableTopics(void*, const PJ_available_topic_t*, uint64_t, PJ_error_t*) noexcept {
    return true;
  }
};

// ---------------------------------------------------------------------------
// CLI plumbing
// ---------------------------------------------------------------------------

struct Args {
  std::string data;
  std::string script;
  std::string source;
  std::string out;
  std::string plugin;
  std::string rule_file;       // portable rule JSON (--rule); its fields are flag defaults
  std::string notify_file;     // notification config JSON (--notify); deploy concern, kept out of the rule
  bool notify_strict = false;  // --notify-strict: a delivery failure forces exit 3
  PJ::sdk::MarkerSeverity fail_on = PJ::sdk::MarkerSeverity::kError;
  bool fail_on_set = false;
  int csv_time_column = -1;  // >=0: use this CSV column as the time axis (else row number)
  bool list_functions = false;
};

void printUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " --data <file> (--script <name|file.lua> | --rule <rule.json>)\n"
            << "                     [--source <topic/field>] [--out <report.json>]\n"
            << "                     [--fail-on info|warning|error|critical]\n"
            << "                     [--notify <config.json>] [--notify-strict]\n"
            << "                     [--csv-time-column <index>] [--plugin <datasource.so>]\n"
            << "       " << argv0 << " --list-functions\n";
}

// Resolve --script: a built-in function name, else a path to a .lua file. Returns
// the code with --SOURCE-- substituted, or nullopt if not found/readable.
std::optional<std::string> resolveScript(const std::string& script, const std::string& source) {
  for (const auto& f : anomaly_core::builtinFunctions()) {
    if (script == f.name) {
      return anomaly_core::substituteSource(f.code, source);
    }
  }
  std::ifstream file(script);
  if (!file) {
    return std::nullopt;
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return anomaly_core::substituteSource(ss.str(), source);
}

}  // namespace

int main(int argc, char* argv[]) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << flag << " needs a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--data") {
      args.data = next("--data");
    } else if (a == "--script") {
      args.script = next("--script");
    } else if (a == "--rule") {
      args.rule_file = next("--rule");
    } else if (a == "--source") {
      args.source = next("--source");
    } else if (a == "--out") {
      args.out = next("--out");
    } else if (a == "--notify") {
      args.notify_file = next("--notify");
    } else if (a == "--notify-strict") {
      args.notify_strict = true;
    } else if (a == "--plugin") {
      args.plugin = next("--plugin");
    } else if (a == "--csv-time-column") {
      args.csv_time_column = std::stoi(next("--csv-time-column"));
    } else if (a == "--fail-on") {
      const auto sev = anomaly_core::severityFromString(next("--fail-on"));
      if (!sev) {
        std::cerr << "Error: --fail-on must be info|warning|error|critical\n";
        return 2;
      }
      args.fail_on = *sev;
      args.fail_on_set = true;
    } else if (a == "--list-functions") {
      args.list_functions = true;
    } else if (a == "-h" || a == "--help") {
      printUsage(argv[0]);
      return 0;
    } else {
      std::cerr << "Error: unknown argument '" << a << "'\n";
      printUsage(argv[0]);
      return 2;
    }
  }

  if (args.list_functions) {
    for (const auto& f : anomaly_core::builtinFunctions()) {
      std::cout << f.name << '\n';
    }
    return 0;
  }

  if (args.data.empty() || (args.script.empty() && args.rule_file.empty())) {
    printUsage(argv[0]);
    return 2;
  }

  // A portable rule file supplies code/source/fail_on; explicit flags override it.
  std::optional<anomaly_core::Rule> rule;
  if (!args.rule_file.empty()) {
    std::ifstream rf(args.rule_file);
    if (!rf) {
      std::cerr << "Error: cannot read rule '" << args.rule_file << "'\n";
      return 2;
    }
    std::stringstream ss;
    ss << rf.rdbuf();
    std::string rerr;
    rule = anomaly_core::ruleFromJson(ss.str(), &rerr);
    if (!rule) {
      std::cerr << "Error: rule '" << args.rule_file << "': " << rerr << "\n";
      return 2;
    }
  }
  const std::string source = !args.source.empty() ? args.source : (rule ? rule->source : std::string{});
  const PJ::sdk::MarkerSeverity fail_on =
      args.fail_on_set ? args.fail_on : (rule ? rule->fail_on : PJ::sdk::MarkerSeverity::kError);

  // Notification config (--notify) is validated UP FRONT so a typo fails fast (exit 2)
  // instead of after a full analysis. It is a deploy concern, separate from the rule.
  std::optional<anomaly_notify::NotifyConfig> notify_cfg;
  if (!args.notify_file.empty()) {
    std::ifstream nf(args.notify_file);
    if (!nf) {
      std::cerr << "Error: cannot read notify config '" << args.notify_file << "'\n";
      return 2;
    }
    std::stringstream ss;
    ss << nf.rdbuf();
    std::string nerr;
    notify_cfg = anomaly_notify::parseConfig(ss.str(), &nerr);
    if (!notify_cfg) {
      std::cerr << "Error: notify config '" << args.notify_file << "': " << nerr << "\n";
      return 2;
    }
  }

  // Pick the DataSource plugin by file extension (.mcap is delegated-ingest and
  // needs the parser routing below; everything else goes through CSV direct ingest).
  const bool is_mcap = std::filesystem::path(args.data).extension() == ".mcap";
  if (args.plugin.empty()) {
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::canonical("/proc/self/exe", ec);
    const std::filesystem::path dir = ec ? std::filesystem::path(argv[0]).parent_path() : self.parent_path();
    const char* soname = is_mcap ? "libmcap_source_plugin.so" : "libcsv_source_plugin.so";
    args.plugin = (dir / soname).string();
  }

  // 1) Resolve the detection code: from the rule, else from --script (name|file).
  std::optional<std::string> code;
  if (rule) {
    code = anomaly_core::substituteSource(rule->code, source);
  } else {
    code = resolveScript(args.script, source);
    if (!code) {
      std::cerr << "Error: --script '" << args.script << "' is neither a built-in nor a readable file\n";
      return 2;
    }
  }

  // 2) Load the datasource plugin + drive it to read the file into our capture host.
  auto library = PJ::DataSourceLibrary::load(args.plugin);
  if (!library) {
    std::cerr << "Error: load datasource plugin '" << args.plugin << "': " << library.error() << "\n";
    return 2;
  }
  auto handle = library->createHandle();
  if (!handle.valid()) {
    std::cerr << "Error: could not instantiate datasource plugin\n";
    return 2;
  }

  CaptureHost capture;
  capture.setPluginDir(std::filesystem::path(args.plugin).parent_path());
  PJ::ServiceRegistryBuilder reg;
  reg.registerService<PJ::sdk::SourceWriteHostService>(capture.writeHost());
  reg.registerService<PJ::sdk::DataSourceRuntimeHostService>(capture.runtimeHost());
  if (auto st = handle.bind(reg.view()); !st) {
    std::cerr << "Error: bind: " << st.error() << "\n";
    return 2;
  }

  std::string config = std::string(R"({"filepath":")") + args.data + R"(")";
  if (!is_mcap && args.csv_time_column >= 0) {
    config += R"(,"time_mode":"column","time_column_index":)" + std::to_string(args.csv_time_column);
  }
  config += "}";  // MCAP: empty selected_topics => the source loads all topics
  if (auto st = handle.loadConfig(config); !st) {
    std::cerr << "Error: loadConfig: " << st.error() << "\n";
    return 2;
  }
  if (auto st = handle.start(); !st) {
    std::cerr << "Error: read '" << args.data << "': " << st.error() << "\n";
    return 2;
  }
  handle.stop();

  if (capture.series.empty()) {
    std::cerr << "Error: no numeric series decoded from '" << args.data << "'\n";
    return 2;
  }

  // Helpful diagnostic if the requested source isn't present.
  if (!source.empty() && capture.series.find(source) == capture.series.end()) {
    std::cerr << "Error: source '" << source << "' not found. Available series:\n";
    for (const auto& n : capture.order) {
      std::cerr << "  " << n << "\n";
    }
    return 2;
  }

  // 3) Run the detection rule through the shared engine.
  anomaly_core::SeriesProvider provider;
  provider.names = capture.order;
  provider.get = [&capture](const std::string& name) -> const anomaly_core::SeriesAccessor* {
    auto it = capture.series.find(name);
    return it == capture.series.end() ? nullptr : &it->second;
  };

  std::string err;
  const std::vector<PJ::sdk::PlotMarker> markers = anomaly_core::runAnomalyScript(*code, provider, &err);
  if (!err.empty()) {
    std::cerr << "Error: Lua: " << err << "\n";
    return 2;
  }

  // 4) Serialize the JSON report; exit code reflects pass/fail.
  anomaly_core::ReportMeta meta;
  meta.file = args.data;
  meta.script = rule ? (rule->name.empty() ? args.rule_file : rule->name) : args.script;
  meta.source = source;
  meta.fail_on = fail_on;
  const anomaly_core::Report report = anomaly_core::markersToReport(markers, meta);

  if (!args.out.empty()) {
    std::ofstream out(args.out);
    if (!out) {
      std::cerr << "Error: cannot write '" << args.out << "'\n";
      return 2;
    }
    out << report.json << "\n";
    std::cerr << (report.failed ? "FAIL" : "PASS") << ": " << report.total << " marker(s) -> " << args.out << "\n";
  } else {
    std::cout << report.json << "\n";
  }

  // 5) Notifications (opt-in). The detection verdict (exit 0/1) is unchanged; a
  // delivery failure is only fatal under --notify-strict, where it maps to exit 3
  // (overriding 0/1) so a pipeline can detect that an alert never went out.
  bool notify_delivery_failed = false;
  if (notify_cfg) {
    const nlohmann::json report_doc = nlohmann::json::parse(report.json, nullptr, /*allow_exceptions=*/false);
    const anomaly_notify::DispatchResult dr = anomaly_notify::dispatch(report.json, report_doc, *notify_cfg);
    for (const auto& e : dr.errors) {
      std::cerr << "Notify error: " << e << "\n";
    }
    if (dr.policyFired()) {
      std::cerr << "Notify: " << dr.succeeded << "/" << dr.fired << " sink(s) delivered\n";
    }
    notify_delivery_failed = !dr.allOk();
  }

  // --notify-strict makes an undelivered alert OVERRIDE the pass/fail verdict: on the
  // common notify_on="fail" policy the alert fires precisely when the run fails, so
  // "anomaly found but nobody was told" is the case strict exists to surface.
  if (args.notify_strict && notify_delivery_failed) {
    return 3;
  }
  return report.failed ? 1 : 0;
}
