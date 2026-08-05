// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "notify.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace anomaly_notify {
namespace {

// Severity vocabulary of the report (matches anomaly_core / the JSON schema). Kept
// local so this module stays decoupled from anomaly_core / pj_base.
constexpr std::array<const char*, 4> kSeverities = {"info", "warning", "error", "critical"};

int severityRank(const std::string& s) {
  for (int i = 0; i < static_cast<int>(kSeverities.size()); ++i) {
    if (s == kSeverities[i]) {
      return i;
    }
  }
  return -1;
}

std::string transportName(Transport t) {
  switch (t) {
    case Transport::kWebhook:
      return "webhook";
    case Transport::kEmail:
      return "email";
    case Transport::kCommand:
      return "command";
  }
  return "?";
}

// A short label for a sink, for error lines (no secrets).
std::string sinkLabel(const Sink& s) {
  switch (s.transport) {
    case Transport::kWebhook:
      return "webhook(" + s.url + ")";
    case Transport::kEmail:
      return "email(" + s.smtp_url + ")";
    case Transport::kCommand:
      return std::string("command(") + (s.exec.empty() ? "" : s.exec.front()) + ")";
  }
  return "sink";
}

// ---- libcurl one-time global init (curl_global_init is not thread-safe; we are
// single-threaded, and dispatch is the only entry point that needs it). ----
struct CurlGlobal {
  CurlGlobal() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
  }
  ~CurlGlobal() {
    curl_global_cleanup();
  }
};
void ensureCurlGlobal() {
  static CurlGlobal g;  // initialized on first dispatch, cleaned up at exit
  (void)g;
}

size_t discardWrite(char*, size_t size, size_t nmemb, void*) {
  return size * nmemb;  // swallow the endpoint's response body
}

// --- webhook: HTTP(S) POST the report JSON ---
bool sendWebhook(const Sink& s, const std::string& body, std::string* err) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    *err = "curl init failed";
    return false;
  }
  curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
  for (const auto& [k, v] : s.headers) {
    hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, s.url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &discardWrite);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "anomaly_runner");
  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    *err = curl_easy_strerror(rc);
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    *err = "HTTP " + std::to_string(http_code);
    return false;
  }
  return true;
}

// --- email: SMTP upload via libcurl ---
struct UploadCtx {
  const std::string* msg;
  size_t offset;
};
size_t readPayload(char* buffer, size_t size, size_t nmemb, void* ctx) {
  auto* u = static_cast<UploadCtx*>(ctx);
  const size_t room = size * nmemb;
  const size_t left = u->msg->size() - u->offset;
  const size_t take = std::min(room, left);
  if (take > 0) {
    std::memcpy(buffer, u->msg->data() + u->offset, take);
    u->offset += take;
  }
  return take;
}
bool sendEmail(const Sink& s, const std::string& report_json, const nlohmann::json& report, std::string* err) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    *err = "curl init failed";
    return false;
  }
  const std::string subject = s.subject.empty() ? defaultSubject(report) : s.subject;
  std::string message = buildEmailMessage(s, subject, report_json);
  UploadCtx ctx{&message, 0};
  curl_slist* rcpts = nullptr;
  for (const auto& t : s.to) {
    rcpts = curl_slist_append(rcpts, t.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, s.smtp_url.c_str());
  if (!s.username.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, s.username.c_str());
  }
  if (!s.password.empty()) {
    curl_easy_setopt(curl, CURLOPT_PASSWORD, s.password.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_MAIL_FROM, s.from.c_str());
  curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpts);
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, &readPayload);
  curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_TRY));  // STARTTLS when offered
  const CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(rcpts);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    *err = curl_easy_strerror(rc);
    return false;
  }
  return true;
}

// --- command: exec argv, pipe the report JSON to its stdin ---
bool runCommand(const Sink& s, const std::string& body, std::string* err) {
  if (s.exec.empty()) {
    *err = "empty command";
    return false;
  }
#ifdef _WIN32
  (void)body;
  *err = "command sink not supported on Windows";
  return false;
#else
  std::array<int, 2> fds{};
  if (pipe(fds.data()) != 0) {
    *err = "pipe failed";
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    *err = "fork failed";
    return false;
  }
  if (pid == 0) {
    // Child: report JSON arrives on stdin.
    dup2(fds[0], STDIN_FILENO);
    close(fds[0]);
    close(fds[1]);
    std::vector<char*> argv;
    argv.reserve(s.exec.size() + 1);
    for (const auto& a : s.exec) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);  // exec failed
  }
  // Parent: feed the body, then reap.
  close(fds[0]);
  signal(SIGPIPE, SIG_IGN);
  size_t off = 0;
  while (off < body.size()) {
    const ssize_t w = write(fds[1], body.data() + off, body.size() - off);
    if (w <= 0) {
      break;
    }
    off += static_cast<size_t>(w);
  }
  close(fds[1]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return true;
  }
  *err = "command exited " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  return false;
#endif
}

}  // namespace

std::string expandEnv(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '$') {
      out.push_back('$');  // "$$" -> literal "$"
      i += 2;
      continue;
    }
    if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
      const size_t end = s.find('}', i + 2);
      if (end != std::string::npos) {
        const std::string var = s.substr(i + 2, end - (i + 2));
        if (const char* val = std::getenv(var.c_str()); val != nullptr) {
          out += val;
        }
        i = end + 1;
        continue;
      }
    }
    out.push_back(s[i++]);
  }
  return out;
}

bool shouldNotify(const nlohmann::json& report, const std::string& notify_on) {
  if (notify_on == "always") {
    return true;
  }
  if (notify_on == "fail") {
    return report.value("status", std::string{}) == "fail";
  }
  // "severity>=<level>": fire if any marker is at or above <level>.
  constexpr const char* kPrefix = "severity>=";
  if (notify_on.rfind(kPrefix, 0) == 0) {
    const int threshold = severityRank(notify_on.substr(std::strlen(kPrefix)));
    if (threshold < 0) {
      return false;
    }
    const auto by_sev =
        report.value("summary", nlohmann::json::object()).value("by_severity", nlohmann::json::object());
    for (int r = threshold; r < static_cast<int>(kSeverities.size()); ++r) {
      if (by_sev.value(kSeverities[r], 0) > 0) {
        return true;
      }
    }
    return false;
  }
  return false;
}

std::string defaultSubject(const nlohmann::json& report) {
  const std::string status = report.value("status", std::string("?"));
  const int total = report.value("summary", nlohmann::json::object()).value("total", 0);
  const std::string file = report.value("file", std::string{});
  std::string upper = status;
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
  std::string subject = "[anomaly] " + upper + ": " + std::to_string(total) + " marker(s)";
  if (!file.empty()) {
    subject += " — " + file;
  }
  return subject;
}

std::string buildEmailMessage(const Sink& sink, const std::string& subject, const std::string& body) {
  std::string to_joined;
  for (size_t i = 0; i < sink.to.size(); ++i) {
    to_joined += (i ? ", " : "") + sink.to[i];
  }
  std::string msg;
  msg += "From: " + sink.from + "\r\n";
  msg += "To: " + to_joined + "\r\n";
  msg += "Subject: " + subject + "\r\n";
  msg += "Content-Type: application/json; charset=utf-8\r\n";
  msg += "\r\n";  // header/body separator
  msg += body;
  return msg;
}

std::optional<NotifyConfig> parseConfig(const std::string& json_text, std::string* error) {
  auto fail = [&](const std::string& m) -> std::optional<NotifyConfig> {
    if (error != nullptr) {
      *error = m;
    }
    return std::nullopt;
  };

  nlohmann::json j = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    return fail("malformed JSON");
  }
  if (!j.is_object()) {
    return fail("config must be a JSON object");
  }

  NotifyConfig cfg;
  cfg.notify_on = j.value("notify_on", std::string("fail"));
  if (cfg.notify_on != "fail" && cfg.notify_on != "always" && cfg.notify_on.rfind("severity>=", 0) != 0) {
    return fail("notify_on must be 'fail', 'always', or 'severity>=<info|warning|error|critical>'");
  }
  if (cfg.notify_on.rfind("severity>=", 0) == 0 && severityRank(cfg.notify_on.substr(10)) < 0) {
    return fail("notify_on severity must be one of info|warning|error|critical");
  }

  const auto sinks = j.value("sinks", nlohmann::json::array());
  if (!sinks.is_array() || sinks.empty()) {
    return fail("config needs a non-empty 'sinks' array");
  }
  auto env = [](const nlohmann::json& v) { return expandEnv(v.is_string() ? v.get<std::string>() : std::string{}); };

  for (const auto& sj : sinks) {
    if (!sj.is_object() || !sj.contains("type")) {
      return fail("each sink needs a 'type'");
    }
    const std::string type = sj.value("type", std::string{});
    Sink s;
    if (type == "webhook") {
      s.transport = Transport::kWebhook;
      s.url = env(sj.value("url", nlohmann::json{}));
      if (s.url.empty()) {
        return fail("webhook sink needs a 'url'");
      }
      // Materialize into a named value: .items() on a temporary json dangles.
      const nlohmann::json headers = sj.value("headers", nlohmann::json::object());
      for (const auto& [k, v] : headers.items()) {
        s.headers.emplace_back(k, env(v));
      }
    } else if (type == "email") {
      s.transport = Transport::kEmail;
      s.smtp_url = env(sj.value("smtp_url", nlohmann::json{}));
      s.from = env(sj.value("from", nlohmann::json{}));
      for (const auto& t : sj.value("to", nlohmann::json::array())) {
        s.to.push_back(env(t));
      }
      s.subject = env(sj.value("subject", nlohmann::json{}));
      s.username = env(sj.value("username", nlohmann::json{}));
      s.password = env(sj.value("password", nlohmann::json{}));
      if (s.smtp_url.empty() || s.from.empty() || s.to.empty()) {
        return fail("email sink needs 'smtp_url', 'from', and a non-empty 'to'");
      }
    } else if (type == "command") {
      s.transport = Transport::kCommand;
      for (const auto& a : sj.value("exec", nlohmann::json::array())) {
        s.exec.push_back(env(a));
      }
      if (s.exec.empty()) {
        return fail("command sink needs a non-empty 'exec' array");
      }
    } else {
      return fail("unknown sink type '" + type + "' (want webhook|email|command)");
    }
    cfg.sinks.push_back(std::move(s));
  }
  return cfg;
}

DispatchResult dispatch(const std::string& report_json, const nlohmann::json& report, const NotifyConfig& cfg) {
  DispatchResult result;
  if (!shouldNotify(report, cfg.notify_on)) {
    return result;  // policy did not fire; nothing attempted
  }
  ensureCurlGlobal();
  for (const auto& s : cfg.sinks) {
    result.fired++;
    std::string err;
    bool ok = false;
    switch (s.transport) {
      case Transport::kWebhook:
        ok = sendWebhook(s, report_json, &err);
        break;
      case Transport::kEmail:
        ok = sendEmail(s, report_json, report, &err);
        break;
      case Transport::kCommand:
        ok = runCommand(s, report_json, &err);
        break;
    }
    if (ok) {
      result.succeeded++;
    } else {
      result.errors.push_back(transportName(s.transport) + " " + sinkLabel(s) + ": " + err);
    }
  }
  return result;
}

}  // namespace anomaly_notify
