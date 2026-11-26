// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "claude_backend.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "claude_sessions.hpp"
#include "cli_locator.hpp"      // locateCli, childEnvFor
#include "cli_probe.hpp"        // probeCliVersion, cannotFindCliMessage
#include "harness_workdir.hpp"  // ensureWorkDir
#include "private_file.hpp"     // writePrivateTempFile / removePrivateTempFile
#include "stream_json.hpp"
#include "subprocess.hpp"
#include "system_prompt.hpp"

namespace assistant_agent {

std::vector<std::string> buildClaudeArgv(
    const std::string& cli_path, const std::string& mcp_config_path, const std::string& system_prompt,
    const std::string& model, const std::string& session_id) {
  std::vector<std::string> argv = {
      cli_path,
      "-p",
      "--output-format",
      "stream-json",
      "--verbose",  // required for stream-json under --print
      // Disables EVERY built-in tool (Bash/Read/Write/...): headless Claude gets ONLY our
      // MCP tools, so it cannot touch the machine outside PlotJuggler's non-destructive
      // surface. `--strict-mcp-config` alone does not do this; it only restricts which MCP
      // servers load. Never relax this: a built-in such as `Read` hands the model the whole
      // filesystem. A feature needing more must be an MCP tool of ours with its own bounds.
      "--tools",
      "",
      // Ignores the machine's user/project/local settings files (an explicit --settings
      // would still apply; we pass none). Without it the panel inherits the user's own
      // coding-session configuration, output style included, and the user-level CLAUDE.md
      // loads as instructions regardless of cwd. Subscription OAuth is untouched. Its tool
      // restrictions are redundant under `--tools ""`.
      "--restricted",
      "--mcp-config",
      mcp_config_path,
      "--strict-mcp-config",
      // Every tool of our one MCP server ("pj", from --mcp-config), nothing else.
      // A server name, never a built-in, so it cannot let one back in.
      "--allowedTools",
      "mcp__pj",
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
      instructions_(assistantInstructions()),
      memory_(memory ? std::move(memory) : std::make_shared<HarnessMemory>()) {}

ClaudeBackend::~ClaudeBackend() {
  removePrivateTempFile(mcp_config_path_);
  // work_dir_ stays: it holds the session state a persisted conversation resumes into.
}

bool ClaudeBackend::ensureWorkDir(std::string& error) {
  return assistant_agent::ensureWorkDir(work_dir_, error);
}

std::string ClaudeBackend::mcpConfigFile(const PJ::sdk::McpEndpoint& endpoint) {
  if (!mcp_config_path_.empty() && endpoint.url == mcp_config_endpoint_.url &&
      endpoint.bearer_token == mcp_config_endpoint_.bearer_token) {
    return mcp_config_path_;
  }
  const nlohmann::json cfg = {
      {"mcpServers",
       {{"pj",
         {{"type", "http"},
          {"url", endpoint.url},
          {"headers", {{"Authorization", "Bearer " + endpoint.bearer_token}}}}}}}};
  std::string path;
  if (!writePrivateTempFile("pj_assistant_mcp_", cfg.dump(), path)) {
    return {};
  }
  removePrivateTempFile(mcp_config_path_);
  mcp_config_path_ = std::move(path);
  mcp_config_endpoint_ = endpoint;
  return mcp_config_path_;
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
  // Curated: `claude --help` names only these four aliases and there is no on-disk
  // catalog like Codex's (codex_models.hpp). `sonnet` leads as the measured default
  // (fastest tier on turn time, zero misses in the 240-cell study).
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
  last_metrics_ = TurnMetrics{};  // a failed turn must not report the previous one's cost

  const auto fail = [&sink](std::string why) {
    sink({BackendEvent::Kind::Error, std::move(why)});
    sink({BackendEvent::Kind::TurnComplete, {}});
  };
  if (!tools.mcp) {
    return fail(tools.mcp.error());
  }
  std::string error;
  if (!ensureWorkDir(error)) {
    return fail(error);
  }
  const std::string mcp_config_path = mcpConfigFile(*tools.mcp);
  if (mcp_config_path.empty()) {
    return fail("could not write the MCP config file");
  }
  const CliLocation loc = locateCli(cli_path_);
  if (loc.path.empty()) {
    return fail(cannotFindCliMessage(cli_path_, loc));
  }

  // Captured before the stream's init record overwrites the memory's id: it turns a
  // 0-turn error into "that session is gone".
  const std::string resume_id = memory_->session_id;
  ListingsTurn listings_turn(*memory_);
  const std::string payload = composePayload(text, tools.features, tools.catalog, *memory_);

  const std::vector<std::string> argv =
      buildClaudeArgv(loc.path, mcp_config_path, instructions_, model_, memory_->session_id);

  NdjsonSplitter splitter;
  std::map<std::string, std::string> tool_names;  // tool_use id -> name, to label the result that answers it
  bool emitted_text = false;
  bool errored = false;
  bool rate_limited = false;  // this turn's; rate_limited_ stays set across turns

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
          listings_turn.answered();
          break;
        case ClaudeEvent::Kind::ToolUse:
          tool_names[ev.tool_id] = prettyToolName(ev.tool_name);
          sink({BackendEvent::Kind::ToolActivity, prettyToolName(ev.tool_name)});
          listings_turn.answered();
          break;
        case ClaudeEvent::Kind::ToolResult: {
          BackendEvent result{BackendEvent::Kind::ToolResult, tool_names[ev.tool_id]};
          result.tool_failed = ev.is_error;
          sink(std::move(result));
          break;
        }
        case ClaudeEvent::Kind::RateLimit:
          if (ev.is_error) {
            rate_limited_ = true;
            rate_limited = true;
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
            // parseClaudeLine already folds "never reached the API" (all-zero usage under
            // this terminal_reason) into is_error; this only supplies the wording.
            sink(
                {BackendEvent::Kind::Error, "the Claude CLI could not reach the API with model '" +
                                                (model_.empty() ? std::string("CLI default") : model_) +
                                                "' (unrecognized model, or the API is down)"});
            errored = true;
          } else if (ev.is_error) {
            // A resumed turn with zero model turns never got past --resume: the CLI has no
            // such session (its stderr says so, but we do not read stderr).
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
      // The message goes via stdin and the MCP config (bearer token) via a private temp
      // file, so nothing sensitive lands on argv, where /proc/<pid>/cmdline exposes it;
      // stdin also keeps a message starting with '-' from being parsed as a flag.
      // childEnvFor(loc) prepends the CLI's directory to PATH so a `claude` found outside
      // this process's PATH (nvm, ~/.local/bin) finds its own interpreter.
      argv, payload, [&](const std::string& chunk) { splitter.feed(chunk, on_line); }, cancel_, work_dir_,
      childEnvFor(loc));
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
  listings_turn.finish(!res.spawned || cancel_.load() || res.exit_code != 0 || errored || rate_limited);
  // Only when the CLI produced a result record; last_metrics_ was reset at the top.
  if (last_metrics_.valid) {
    sink({BackendEvent::Kind::Metrics, {}, last_metrics_});
  }
  sink({BackendEvent::Kind::TurnComplete, {}});
}

}  // namespace assistant_agent
