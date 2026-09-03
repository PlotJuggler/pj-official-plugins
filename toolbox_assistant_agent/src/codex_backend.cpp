// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "codex_backend.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // mkstemp/write/close/unlink for the private instructions file
#endif

#include <string>
#include <utility>
#include <vector>

#include "cli_probe.hpp"  // probeCliVersion
#include "codex_sessions.hpp"
#include "codex_stream.hpp"
#include "harness_workdir.hpp"
#include "private_file.hpp"  // writePrivateTempFile
#include "stream_json.hpp"   // NdjsonSplitter (generic; not Claude-specific)
#include "subprocess.hpp"
#include "system_prompt.hpp"

namespace assistant_agent {

std::vector<std::string> buildCodexArgv(
    const std::string& cli_path, const std::string& workdir, const std::string& mcp_url,
    const std::string& token_env_name, const std::string& instructions_path, const std::string& model,
    const std::string& session_id) {
  const bool resuming = !session_id.empty();
  std::vector<std::string> argv = {cli_path, "exec"};
  if (resuming) {
    // No -C on resume: Codex resolves the thread from its own session store
    // regardless of cwd (verified 2026-09-03 on CLI 0.153.0).
    argv.push_back("resume");
    argv.push_back(session_id);
  }
  argv.push_back("--json");
  argv.push_back("--skip-git-repo-check");
  argv.push_back("--ignore-user-config");
  argv.push_back("--ignore-rules");
  if (!resuming) {
    argv.push_back("-C");
    argv.push_back(workdir);
  }
  // The tool-withholding spine. There is no `--tools ""` equivalent in Codex:
  // every call, ours included, runs inside Code Mode (a JS host with no
  // require/process/fetch), so disabling it would also disable our own MCP
  // tools — it stays on. What withholds the rest is these `-c` config values
  // plus the seven `--disable`s below. Not a default to be relaxed; see the
  // "not negotiable" note in claude_backend.cpp's own buildClaudeArgv.
  const auto addConfig = [&argv](const std::string& key_value) {
    argv.push_back("-c");
    argv.push_back(key_value);
  };
  addConfig("features.shell_tool=false");
  addConfig("features.unified_exec=false");
  addConfig(R"(web_search="disabled")");
  addConfig("tools.view_image=false");
  addConfig(R"(sandbox_mode="read-only")");
  addConfig(R"(approval_policy="never")");
  for (const char* name :
       {"view_image", "memories", "shell_snapshot", "multi_agent", "plugins", "apps", "skill_search"}) {
    argv.push_back("--disable");
    argv.push_back(name);
  }
  // Replaces Codex's own "You are Codex" persona with ours.
  addConfig("model_instructions_file=\"" + instructions_path + "\"");
  // Our tools reach the model through this MCP server; without
  // default_tools_approval_mode="approve" every call fails outright with
  // "requires approval, but approval policy is never" (approval_policy=
  // "never" above disables Codex's OWN interactive prompt, it does not grant
  // MCP tools an exemption on its own).
  addConfig("mcp_servers.pj.url=\"" + mcp_url + "\"");
  addConfig("mcp_servers.pj.bearer_token_env_var=\"" + token_env_name + "\"");
  addConfig("mcp_servers.pj.required=true");
  addConfig(R"(mcp_servers.pj.default_tools_approval_mode="approve")");
  if (!model.empty()) {
    argv.push_back("-m");
    argv.push_back(model);
  }
  argv.push_back("-");  // the prompt is read from stdin, never an argv entry
  return argv;
}

CodexBackend::CodexBackend(std::string cli_path, std::string model, std::shared_ptr<HarnessMemory> memory)
    : cli_path_(cli_path.empty() ? "codex" : std::move(cli_path)),
      model_(std::move(model)),
      memory_(memory ? std::move(memory) : std::make_shared<HarnessMemory>()) {}

CodexBackend::~CodexBackend() {
  if (!instructions_path_.empty()) {
    unlink(instructions_path_.c_str());
  }
  // mcp_'s own destructor (McpLoopback) tears down the loopback server; it
  // never wrote a config FILE for Codex (url()/token() go straight into argv
  // and env), so there is nothing of its own to unlink here. work_dir_ is
  // deliberately left in place -- it holds Codex's session state, which is
  // exactly what `codex exec resume` comes back to.
}

bool CodexBackend::ensureWorkDir(std::string& error) {
  return assistant_agent::ensureWorkDir(work_dir_, error);
}

bool CodexBackend::ensureInstructionsFile(std::string& error) {
  if (!instructions_path_.empty()) {
    return true;
  }
  const std::string contents =
      std::string(kSystemPrompt) + "\n\nNever spawn sub-agents or use collaboration tools; do the work in this turn.";
  if (!writePrivateTempFile("/tmp/pj_assistant_codex_", contents, instructions_path_)) {
    error = "could not write the Codex instructions file";
    return false;
  }
  return true;
}

std::string CodexBackend::name() const {
  return "Codex" + (model_.empty() ? std::string{} : ": " + model_);
}

void CodexBackend::cancel() {
  cancel_.store(true);
}

BackendTestResult CodexBackend::testConnection() const {
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
                   " (is the Codex CLI installed and logged in?)"};
  }
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) {
    version.pop_back();
  }
  return {true, "found " + version};
}

std::vector<ConversationSummary> CodexBackend::listConversations() {
  std::string error;
  if (!ensureWorkDir(error)) {
    return {};
  }
  return assistant_agent::listCodexConversations(codexSessionsDir(), work_dir_);
}

std::vector<ChatMessage> CodexBackend::loadTranscript(const std::string& id) {
  std::string error;
  if (!ensureWorkDir(error)) {
    return {};
  }
  return assistant_agent::loadCodexTranscript(codexSessionsDir(), id);
}

bool CodexBackend::deleteConversation(const std::string& id) {
  std::string error;
  if (!ensureWorkDir(error)) {
    return false;
  }
  return assistant_agent::deleteCodexConversation(codexSessionsDir(), id);
}

void CodexBackend::sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) {
  cancel_.store(false);
  last_metrics_ = TurnMetrics{};  // per-turn, so a failed turn cannot report the previous one's cost

  std::string error;
  if (!mcp_.ensure(tools, error) || !ensureWorkDir(error) || !ensureInstructionsFile(error)) {
    sink({BackendEvent::Kind::Error, error});
    sink({BackendEvent::Kind::TurnComplete, {}});
    return;
  }

  // Whether this turn carries `resume`, captured before the stream's
  // thread.started record overwrites the memory's id: it is what turns a
  // silent, empty-stdout exit 1 into "that session is gone" instead of a
  // generic failure.
  const std::string resume_id = memory_->session_id;
  const std::string payload = composePayload(text, tools.catalog, *memory_);

  const std::vector<std::string> argv = buildCodexArgv(
      cli_path_, work_dir_, mcp_.url(), kCodexMcpTokenEnvVar, instructions_path_, model_, memory_->session_id);
  // The bearer token travels only through the child's environment, never
  // argv (/proc/<pid>/cmdline is world-readable) -- the same reason
  // ClaudeBackend keeps its own token inside a private --mcp-config file.
  const std::vector<std::pair<std::string, std::string>> extra_env = {{kCodexMcpTokenEnvVar, mcp_.token()}};

  NdjsonSplitter splitter;
  bool saw_thread_started = false;
  bool in_turn = false;
  bool errored = false;

  auto on_line = [&](const std::string& line) {
    const CodexEvent ev = parseCodexLine(line);
    switch (ev.kind) {
      case CodexEvent::Kind::ThreadStarted:
        saw_thread_started = true;
        if (!ev.session_id.empty()) {
          memory_->session_id = ev.session_id;
        }
        break;
      case CodexEvent::Kind::TurnStarted:
        in_turn = true;
        break;
      case CodexEvent::Kind::AssistantText:
        sink({BackendEvent::Kind::AssistantText, ev.text});
        break;
      case CodexEvent::Kind::ToolCallStarted:
        sink({BackendEvent::Kind::ToolActivity, prettyToolName(ev.tool_name)});
        break;
      case CodexEvent::Kind::ToolCallFailed:
        sink({BackendEvent::Kind::ToolActivity, ev.tool_name + ": " + ev.text});
        break;
      case CodexEvent::Kind::ErrorItem:
        // Codex emits the same item.completed/error shape for a startup
        // notice ("Code Mode is unavailable...", printed before
        // turn.started) as for a genuine mid-turn failure; `in_turn` is the
        // only thing that tells them apart.
        if (in_turn) {
          sink({BackendEvent::Kind::Error, ev.text.empty() ? "Codex reported an error" : ev.text});
          errored = true;
        }
        break;
      case CodexEvent::Kind::TurnFailed:
        sink({BackendEvent::Kind::Error, ev.text.empty() ? "Codex reported an error" : ev.text});
        errored = true;
        break;
      case CodexEvent::Kind::TurnCompleted:
        if (ev.metrics.valid) {
          last_metrics_ = ev.metrics;
        }
        break;
      case CodexEvent::Kind::Ignored:
        break;
    }
  };

  auto res = runProcess(
      argv, payload, [&](const std::string& chunk) { splitter.feed(chunk, on_line); }, cancel_, work_dir_, extra_env);
  splitter.flush(on_line);

  if (!res.spawned) {
    sink({BackendEvent::Kind::Error, "cannot start Codex CLI '" + cli_path_ + "': " + res.error});
  } else if (cancel_.load()) {
    sink({BackendEvent::Kind::Error, "cancelled"});
  } else if (!resume_id.empty() && !saw_thread_started) {
    // Verbatim what the CLI does for `resume <id>` when it has no such
    // thread: exit 1, stdout completely empty (the reason is on stderr only,
    // which is discarded) -- the one failure where retrying cannot help.
    BackendEvent err{
        BackendEvent::Kind::Error, "this conversation can no longer be resumed: Codex has no session " + resume_id};
    err.resume_failed = true;
    sink(err);
  } else if (res.exit_code != 0 && !errored) {
    sink(
        {BackendEvent::Kind::Error,
         "Codex CLI exited " + std::to_string(res.exit_code) + " (check it is installed and logged in via Settings)"});
  }
  if (last_metrics_.valid) {
    sink({BackendEvent::Kind::Metrics, {}, last_metrics_});
  }
  sink({BackendEvent::Kind::TurnComplete, {}});
}

}  // namespace assistant_agent
