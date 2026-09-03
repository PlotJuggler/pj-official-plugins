// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "claude_backend.hpp"

#include <string>
#include <utility>
#include <vector>

#include "claude_sessions.hpp"  // listConversations/loadTranscript/deleteConversation
#include "cli_probe.hpp"        // probeCliVersion
#include "harness_workdir.hpp"  // ensureWorkDir
#include "stream_json.hpp"
#include "subprocess.hpp"
#include "system_prompt.hpp"

namespace assistant_agent {

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
      // Ignore the machine's user/project/local settings files (an explicit
      // --settings would still apply; we pass none). Without this the panel
      // inherits whatever the user configured for their own coding sessions —
      // output style included — and, worse, the user-level CLAUDE.md loads as
      // instructions regardless of cwd (verified 2026-08-31 on CLI 2.1.251:
      // the same prompt answers "# Global rules" without the flag and NONE
      // with it). Subscription OAuth is untouched. Its tool restrictions are
      // redundant under `--tools ""` — that redundancy is fine.
      "--restricted",
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

ClaudeBackend::ClaudeBackend(std::string cli_path, std::string model, std::shared_ptr<HarnessMemory> memory)
    : cli_path_(cli_path.empty() ? "claude" : std::move(cli_path)),
      model_(std::move(model)),
      memory_(memory ? std::move(memory) : std::make_shared<HarnessMemory>()) {}

ClaudeBackend::~ClaudeBackend() = default;
// mcp_'s destructor (McpLoopback) unlinks the --mcp-config temp file. work_dir_
// is deliberately left in place: it holds the CLI's session state, which is
// exactly what a persisted conversation resumes into.

bool ClaudeBackend::ensureWorkDir(std::string& error) {
  return assistant_agent::ensureWorkDir(work_dir_, error);
}

std::string ClaudeBackend::name() const {
  return "Claude Code" + (model_.empty() ? std::string{} : ": " + model_);
}

void ClaudeBackend::cancel() {
  cancel_.store(true);
}

BackendTestResult ClaudeBackend::testConnection() const {
  return probeCliVersion(cli_path_, "Claude");
}

std::vector<ModelChoice> ClaudeBackend::listModels() {
  // Curated, not read off disk: `claude --help` names these four aliases and
  // nothing more, and there is no on-disk catalog the way Codex maintains
  // one (codex_models.hpp). `sonnet` leads because it is the measured
  // default (docs/BENCHMARKS.md: matches the fastest tier on turn time with
  // zero misses in the 240-cell study) -- the same reasoning kBackends[0]'s
  // default_model comment gives, said here for the picker instead.
  return {
      {"sonnet", "sonnet — fast; the measured default"},
      {"opus", "opus"},
      {"fable", "fable"},
      {"haiku", "haiku"},
  };
}

std::vector<ConversationSummary> ClaudeBackend::listConversations() {
  std::string error;
  if (!ensureWorkDir(error)) {
    return {};
  }
  return assistant_agent::listConversations(claudeSessionsDir(work_dir_));
}

std::vector<ChatMessage> ClaudeBackend::loadTranscript(const std::string& id) {
  std::string error;
  if (!ensureWorkDir(error)) {
    return {};
  }
  return assistant_agent::loadTranscript(claudeSessionsDir(work_dir_), id);
}

bool ClaudeBackend::deleteConversation(const std::string& id) {
  std::string error;
  if (!ensureWorkDir(error)) {
    return false;
  }
  return assistant_agent::deleteConversation(claudeSessionsDir(work_dir_), id);
}

void ClaudeBackend::sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) {
  cancel_.store(false);
  last_metrics_ = TurnMetrics{};  // per-turn, so a failed turn cannot report the previous one's cost

  std::string mcp_error;
  if (!mcp_.ensure(tools, mcp_error) || !ensureWorkDir(mcp_error)) {
    sink({BackendEvent::Kind::Error, mcp_error});
    sink({BackendEvent::Kind::TurnComplete, {}});
    return;
  }
  const std::string mcp_config_path = mcp_.configFilePath();
  if (mcp_config_path.empty()) {
    sink({BackendEvent::Kind::Error, "could not write the MCP config file"});
    sink({BackendEvent::Kind::TurnComplete, {}});
    return;
  }

  // Whether this turn carries --resume, captured before the stream's init
  // record overwrites the memory's id: it is what turns a 0-turn error into
  // "that session is gone" instead of a generic failure.
  const std::string resume_id = memory_->session_id;
  const std::string payload = composePayload(text, tools.catalog, *memory_);

  const std::vector<std::string> argv = buildClaudeArgv(
      cli_path_, mcp_config_path, allowedToolsArg(*tools.registry), kSystemPrompt, model_, memory_->session_id);

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
          if (ev.is_error && ev.terminal_reason == "api_error") {
            // parseClaudeLine folds "the CLI never reached the API at all"
            // (an all-zero usage block under this terminal_reason) into
            // is_error itself -- this only has to supply the wording.
            sink(
                {BackendEvent::Kind::Error, "the Claude CLI could not reach the API with model '" +
                                                (model_.empty() ? std::string("CLI default") : model_) +
                                                "' (unrecognized model, or the API is down)"});
            errored = true;
          } else if (ev.is_error) {
            // A resumed turn that ran zero model turns never got past
            // --resume: the CLI has no such session any more (its stderr says
            // "No conversation found with session ID", but stderr is not ours
            // to read). Every other error keeps the CLI's own wording.
            BackendEvent err{BackendEvent::Kind::Error, ev.text.empty() ? "Claude reported an error" : ev.text};
            if (!resume_id.empty() && ev.num_turns == 0) {
              err.text = "this conversation can no longer be resumed: Claude Code has no session " + resume_id;
              err.resume_failed = true;
            }
            sink(err);
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
      argv, payload, [&](const std::string& chunk) { splitter.feed(chunk, on_line); }, cancel_, work_dir_);
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
