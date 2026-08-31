// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// notify — the runner's configurable notification layer (Task F, deliverable #6).
//
// Opt-in via `--notify <config.json>`: after the analysis, the report is delivered
// to one or more SINKS (webhook / email / command) when the configured policy
// fires. Notifications are a DEPLOYMENT concern, kept OUT of the portable rule JSON
// so a rule stays clean/shareable and webhook secrets live only on the server.
//
// The module is deliberately decoupled from anomaly_core: it operates on the
// report's JSON text (the contract), not on the marker types, so the pure parts
// (config parse, ${ENV} expansion, the fire predicate, payload building) unit-test
// with no SDK/network. Only dispatch() touches libcurl (HTTP POST + SMTP) and
// fork/exec (the command sink).

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anomaly_notify {

/// Where a single notification is delivered.
enum class Transport { kWebhook, kEmail, kCommand };

/// One delivery target. Only the fields for its `transport` are meaningful.
struct Sink {
  Transport transport;

  // --- webhook ---
  std::string url;                                           ///< HTTP(S) endpoint
  std::vector<std::pair<std::string, std::string>> headers;  ///< extra request headers (auth, etc.)

  // --- email (SMTP via libcurl) ---
  std::string smtp_url;         ///< "smtp://host:port" or "smtps://host:port"
  std::string from;             ///< envelope + From: address
  std::vector<std::string> to;  ///< recipients
  std::string subject;          ///< Subject: line (a default is built if empty)
  std::string username;         ///< optional SMTP-AUTH user
  std::string password;         ///< optional SMTP-AUTH password (use ${ENV} in the file)

  // --- command (exec, the escape hatch for existing alerting infra) ---
  std::vector<std::string> exec;  ///< argv; the report JSON is piped to the child's stdin
};

/// Parsed `notify.json`. `notify_on` gates ALL sinks at once.
struct NotifyConfig {
  /// "fail" (status==fail) | "always" | "severity>=<info|warning|error|critical>".
  std::string notify_on = "fail";
  std::vector<Sink> sinks;
};

/// Outcome of a dispatch() call.
struct DispatchResult {
  int fired = 0;                    ///< sinks whose policy fired (= attempted deliveries)
  int succeeded = 0;                ///< deliveries that reported success
  std::vector<std::string> errors;  ///< one human-readable line per failed delivery
  bool policyFired() const {
    return fired > 0;
  }
  bool allOk() const {
    return succeeded == fired;
  }
};

// ---------------------------------------------------------------------------
// Pure helpers (no network, no libcurl) — directly unit-testable.
// ---------------------------------------------------------------------------

/// Expand every `${VAR}` in `s` from the environment (unset -> empty string). A
/// literal `$$` yields a single `$`. Used so secrets come from the env, not the file.
[[nodiscard]] std::string expandEnv(const std::string& s);

/// Parse + validate a notify config, expanding `${ENV}` in every string value.
/// Returns nullopt and sets `*error` (if non-null) on malformed JSON, an unknown
/// `notify_on`, an unknown sink `type`, or a sink missing its required fields.
[[nodiscard]] std::optional<NotifyConfig> parseConfig(const std::string& json_text, std::string* error);

/// True if `notify_on` says we should deliver, given the parsed report document.
/// "always" -> always; "fail" -> report["status"]=="fail"; "severity>=X" -> any
/// marker at severity >= X (read from report["summary"]["by_severity"]).
[[nodiscard]] bool shouldNotify(const nlohmann::json& report, const std::string& notify_on);

/// Default subject when a sink leaves `subject` empty, e.g.
/// "[anomaly] FAIL: 7 marker(s) — run.csv".
[[nodiscard]] std::string defaultSubject(const nlohmann::json& report);

/// Build the RFC 5322 message text (From/To/Subject headers + body) an SMTP sink
/// uploads. `body` is appended after the headers (typically the report JSON).
[[nodiscard]] std::string buildEmailMessage(const Sink& sink, const std::string& subject, const std::string& body);

// ---------------------------------------------------------------------------
// Dispatch (touches libcurl + fork/exec).
// ---------------------------------------------------------------------------

/// Deliver `report_json` to every sink in `cfg`, IF `cfg.notify_on` fires for
/// `report` (the parsed form of `report_json`). Never throws; per-sink failures are
/// collected in the result's `errors`. A non-firing policy returns fired==0.
[[nodiscard]] DispatchResult dispatch(
    const std::string& report_json, const nlohmann::json& report, const NotifyConfig& cfg);

}  // namespace anomaly_notify
