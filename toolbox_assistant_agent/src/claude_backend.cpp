// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "claude_backend.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // mkstemp/write/close/unlink for the private MCP config file
#endif

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "stream_json.hpp"
#include "subprocess.hpp"
#include "system_prompt.hpp"

namespace assistant_agent {
namespace {

// Strip the "mcp__pj__" prefix Claude prepends to MCP tool names, for a cleaner
// transcript line ("mcp__pj__list_topics" -> "list_topics").
std::string prettyToolName(const std::string& name) {
  const std::string prefix = "mcp__pj__";
  return name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : name;
}

// Comma-separated allowlist of every tool, MCP-namespaced, so Claude may call
// them without an interactive permission prompt (headless has no UI to grant).
}  // namespace

std::string allowedToolsArg(const ToolRegistry& registry) {
  std::string out;
  for (const auto& spec : registry.tools()) {
    if (!out.empty()) {
      out += ",";
    }
    out += "mcp__pj__" + spec.name;
  }
  return out;
}

std::vector<std::string> buildClaudeArgv(
    const std::string& cli_path, const std::string& mcp_config_path, const std::string& allowed_tools,
    const std::string& system_prompt, const std::string& model, const std::string& session_id) {
  std::vector<std::string> argv = {
      cli_path,
      "-p",
      "--output-format",
      "stream-json",
      "--verbose",  // required for stream-json under --print
      // Disable EVERY built-in tool (Bash/Read/Write/...). This is the safety
      // spine: headless Claude gets ONLY our MCP tools, so it cannot touch the
      // machine outside PlotJuggler's non-destructive surface. `--strict-mcp-config`
      // alone does not do this — it only restricts which MCP servers load.
      //
      // Not a default to be relaxed. Granting a built-in tool — `Read` to let the
      // model look at an exported image is the tempting one — hands it the whole
      // filesystem, and the guarantee that this plugin cannot touch the user's
      // machine is gone. Any such feature has to arrive through an MCP tool of
      // ours with its own bounds, not by widening this.
      "--tools",
      "",
      "--mcp-config",
      mcp_config_path,
      "--strict-mcp-config",
      "--allowedTools",
      allowed_tools,
      "--append-system-prompt",
      system_prompt,
  };
  if (!model.empty()) {
    argv.push_back("--model");
    argv.push_back(model);
  }
  if (!session_id.empty()) {
    argv.push_back("--resume");
    argv.push_back(session_id);
  }
  return argv;
}

ClaudeBackend::ClaudeBackend(std::string cli_path, std::string model, std::shared_ptr<ClaudeMemory> memory)
    : cli_path_(cli_path.empty() ? "claude" : std::move(cli_path)),
      model_(std::move(model)),
      memory_(memory ? std::move(memory) : std::make_shared<ClaudeMemory>()) {}

ClaudeBackend::~ClaudeBackend() {
  if (!mcp_config_path_.empty()) {
    unlink(mcp_config_path_.c_str());
  }
}

std::string ClaudeBackend::name() const {
  return "Claude Code" + (model_.empty() ? std::string{} : ": " + model_);
}

void ClaudeBackend::cancel() {
  cancel_.store(true);
}

BackendTestResult ClaudeBackend::testConnection() const {
  std::atomic<bool> no_cancel{false};
  std::string version;
  auto res = runProcess(
      {cli_path_, "--version"}, /*stdin_data=*/{}, [&](const std::string& chunk) { version += chunk; }, no_cancel);
  if (!res.spawned) {
    return {false, "cannot run '" + cli_path_ + "': " + res.error};
  }
  if (res.exit_code != 0) {
    return {
        false, "'" + cli_path_ + " --version' exited " + std::to_string(res.exit_code) +
                   " (is the Claude CLI installed and logged in?)"};
  }
  // Trim trailing newline for a tidy transcript line.
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) {
    version.pop_back();
  }
  return {true, "found " + version};
}

bool ClaudeBackend::ensureMcpServer(const TurnTools& tools, std::string& error) {
  if (mcp_) {
    return true;
  }
  if (tools.registry == nullptr || !tools.invoke) {
    error = "no tool registry/invoker for this turn";
    return false;
  }
  mcp_ = std::make_unique<McpHttpServer>(*tools.registry, tools.invoke);
  if (!mcp_->start()) {
    mcp_.reset();
    error = "could not bind a local MCP port";
    return false;
  }
  // The config carries the per-session bearer token; hand it to the CLI as a
  // private file rather than a command-line argument (argv is world-readable
  // via /proc/<pid>/cmdline). mkstemp creates it 0600.
  char tmpl[] = "/tmp/pj_assistant_mcp_XXXXXX";
  const int fd = mkstemp(tmpl);
  if (fd < 0) {
    mcp_->stop();
    mcp_.reset();
    error = "could not create the MCP config file";
    return false;
  }
  const std::string cfg = mcp_->mcpConfigJson();
  ssize_t off = 0;
  while (off < static_cast<ssize_t>(cfg.size())) {
    const ssize_t w = write(fd, cfg.data() + off, cfg.size() - static_cast<std::size_t>(off));
    if (w <= 0) {
      break;
    }
    off += w;
  }
  close(fd);
  if (off != static_cast<ssize_t>(cfg.size())) {
    unlink(tmpl);
    mcp_->stop();
    mcp_.reset();
    error = "could not write the MCP config file";
    return false;
  }
  mcp_config_path_ = tmpl;
  return true;
}

void ClaudeBackend::sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) {
  cancel_.store(false);
  last_metrics_ = TurnMetrics{};  // per-turn, so a failed turn cannot report the previous one's cost

  std::string mcp_error;
  if (!ensureMcpServer(tools, mcp_error)) {
    sink({BackendEvent::Kind::Error, mcp_error});
    sink({BackendEvent::Kind::TurnComplete, {}});
    return;
  }

  // Prepend the catalog listing to the user's message, but only when it is new
  // to this conversation. --resume replays the whole history, so a listing sent
  // once stays visible on every later turn; sending it again would just pay for
  // the same text twice. A listing that has CHANGED does get re-sent — that is
  // how the model learns the loaded data is not what it was told earlier.
  std::string payload = text;
  if (!tools.catalog.empty() && tools.catalog != memory_->sent_catalog) {
    const bool first = memory_->sent_catalog.empty();
    payload = tools.catalog + "\n" +
              (first ? "" : "(The loaded data changed; the listing above replaces the earlier one.)\n") + "\n" + text;
    memory_->sent_catalog = tools.catalog;
  }

  const std::vector<std::string> argv = buildClaudeArgv(
      cli_path_, mcp_config_path_, allowedToolsArg(*tools.registry), kSystemPrompt, model_, memory_->session_id);

  NdjsonSplitter splitter;
  bool emitted_text = false;
  bool errored = false;

  auto on_line = [&](const std::string& line) {
    for (const auto& ev : parseClaudeLine(line)) {
      switch (ev.kind) {
        case ClaudeEvent::Kind::Init:
          if (!ev.session_id.empty()) {
            memory_->session_id = ev.session_id;
          }
          break;
        case ClaudeEvent::Kind::AssistantText:
          sink({BackendEvent::Kind::AssistantText, ev.text});
          emitted_text = true;
          break;
        case ClaudeEvent::Kind::ToolUse:
          sink({BackendEvent::Kind::ToolActivity, prettyToolName(ev.tool_name)});
          break;
        case ClaudeEvent::Kind::RateLimit:
          if (ev.is_error) {
            rate_limited_ = true;
            sink({BackendEvent::Kind::Error, "usage limit reached (" + ev.text + ")"});
          }
          break;
        case ClaudeEvent::Kind::Result:
          if (ev.metrics.valid) {
            last_metrics_ = ev.metrics;
          }
          if (!ev.session_id.empty()) {
            memory_->session_id = ev.session_id;
          }
          if (ev.is_error) {
            sink({BackendEvent::Kind::Error, ev.text.empty() ? "Claude reported an error" : ev.text});
            errored = true;
          } else if (!emitted_text && !ev.text.empty()) {
            // Tool-only turns carry their summary only in the result record.
            sink({BackendEvent::Kind::AssistantText, ev.text});
          }
          break;
        case ClaudeEvent::Kind::Ignored:
          break;
      }
    }
  };

  auto res = runProcess(
      // The message goes via stdin, and the MCP config (which carries the
      // bearer token) via a private temp file: nothing sensitive lands on the
      // command line, where /proc/<pid>/cmdline exposes it to every local
      // user. stdin also keeps a message starting with '-' from being parsed
      // as a CLI flag.
      argv, payload, [&](const std::string& chunk) { splitter.feed(chunk, on_line); }, cancel_);
  splitter.flush(on_line);

  if (!res.spawned) {
    sink({BackendEvent::Kind::Error, "cannot start Claude CLI '" + cli_path_ + "': " + res.error});
  } else if (cancel_.load()) {
    sink({BackendEvent::Kind::Error, "cancelled"});
  } else if (res.exit_code != 0 && !errored) {
    sink(
        {BackendEvent::Kind::Error,
         "Claude CLI exited " + std::to_string(res.exit_code) + " (check it is installed and logged in via Settings)"});
  }
  // Report what the turn cost before closing it out. Only when the CLI actually
  // produced a result record: a turn that died early has no price to quote, and
  // last_metrics_ was reset at the top so it cannot repeat the previous one's.
  if (last_metrics_.valid) {
    sink({BackendEvent::Kind::Metrics, {}, last_metrics_});
  }
  sink({BackendEvent::Kind::TurnComplete, {}});
}

}  // namespace assistant_agent
