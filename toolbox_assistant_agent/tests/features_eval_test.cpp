// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Feature-awareness benchmark runner. Asks the real CLI the product questions
// in docs/benchmarks/features/questions.json, once per fixture the question
// names, and writes one JSONL line per reply for score.py in the same
// directory. Opt-in, like ClaudeSmoke/CodexSmoke: it needs a logged-in CLI and
// spends the user's subscription, so it is skipped unless
// ASSISTANT_FEATURES_EVAL is set, and CI never runs it.
//
//   ASSISTANT_FEATURES_EVAL=claude|codex   backend to drive (required)
//   ASSISTANT_FEATURES_EVAL_MODEL=<model>  default: sonnet for Claude, the CLI's own for Codex
//   ASSISTANT_FEATURES_EVAL_REPEAT=<n>     conversations per question and fixture (default 1)
//   ASSISTANT_FEATURES_EVAL_OUT=<path>     JSONL output (default features_eval.jsonl, overwritten)
//   ASSISTANT_FEATURES_EVAL_BASELINE=1     no product policy and no features block: today's behaviour
//   ASSISTANT_FEATURES_EVAL_ONLY=<ids>     comma-separated question ids
//   ASSISTANT_CLAUDE_CLI / ASSISTANT_CODEX_CLI   CLI path override, as in the smoke tests
//
// Each conversation is what the panel would send: the real backend (same
// argv, so the same built-in tools withheld), the fixture rendered by the
// real featuresDigest, an empty catalog, and composePayload inside the
// backend. A question with a follow-up ("follows" in questions.json) is a
// two-turn conversation. Run the binary directly to see progress:
//   ASSISTANT_FEATURES_EVAL=claude ASSISTANT_FEATURES_EVAL_OUT=/tmp/claude.jsonl \
//     build/toolbox_assistant_agent/Release/toolbox_assistant_agent/toolbox_assistant_agent_features_eval_test
//   python3 -I docs/benchmarks/features/score.py /tmp/claude.jsonl
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/number_parse.hpp>
#include <pj_base/sdk/platform.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "claude_backend.hpp"
#include "codex_backend.hpp"
#include "features_digest.hpp"
#include "support/file_io.hpp"
#include "system_prompt.hpp"
#include "tool_registry.hpp"

#ifndef ASSISTANT_FEATURES_BENCH_DIR
#error "ASSISTANT_FEATURES_BENCH_DIR must be defined by CMake"
#endif

namespace {

using assistant_agent::BackendEvent;
using assistant_agent::HarnessMemory;
using assistant_agent::ToolContext;
using assistant_agent::ToolRegistry;
using assistant_agent::TurnTools;
using nlohmann::json;

// What catalogDigest says with nothing loaded: product questions are asked
// before any data is opened.
constexpr const char* kEmptyCatalog = "Loaded data: nothing is loaded yet.";

std::set<std::string> splitIds(const std::string& csv) {
  std::set<std::string> out;
  std::stringstream in(csv);
  for (std::string id; std::getline(in, id, ',');) {
    const auto b = id.find_first_not_of(' ');
    const auto e = id.find_last_not_of(' ');
    if (b != std::string::npos) {
      out.insert(id.substr(b, e - b + 1));
    }
  }
  return out;
}

struct Turn {
  std::string reply;
  std::vector<std::string> tool_calls;
  assistant_agent::TurnMetrics metrics;
  std::string error;
};

template <class Backend>
Turn runTurn(Backend& backend, const std::string& text, const TurnTools& tools) {
  Turn turn;
  std::mutex mu;
  backend.sendUserMessage(text, tools, [&](BackendEvent e) {
    std::lock_guard<std::mutex> lk(mu);
    switch (e.kind) {
      case BackendEvent::Kind::AssistantText:
        turn.reply += e.text;
        break;
      case BackendEvent::Kind::ToolActivity:
        // "name: error" is Codex reporting a call already counted when it
        // started (codex_backend.cpp), not a second call.
        if (e.text.find(": ") == std::string::npos) {
          turn.tool_calls.push_back(e.text);
        }
        break;
      case BackendEvent::Kind::Error:
        turn.error += (turn.error.empty() ? "" : "; ") + e.text;
        break;
      default:
        break;
    }
  });
  turn.metrics = backend.lastTurnMetrics();
  return turn;
}

struct Settings {
  std::string backend;
  std::string cli;
  std::string model;
  int repeat = 1;
  std::string out_path;
  bool baseline = false;
  std::set<std::string> only;
};

template <class Backend>
void runEval(const Settings& settings) {
  const std::string bench = ASSISTANT_FEATURES_BENCH_DIR;
  const json questions =
      json::parse(assistant_agent::testing::readFileToString(bench + "/questions.json"))["questions"];

  std::map<std::string, std::vector<json>> follow_ups;
  for (const auto& q : questions) {
    if (q.contains("follows")) {
      follow_ups[q["follows"].get<std::string>()].push_back(q);
    }
  }
  const auto selected = [&](const json& q) {
    return settings.only.empty() || settings.only.count(q["id"].get<std::string>()) > 0;
  };

  std::ofstream out(settings.out_path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out) << "cannot write " << settings.out_path;

  ToolRegistry registry;
  PJ::testing::ToolboxTestStore store;  // nothing loaded
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());
  const std::string model_label = settings.model.empty() ? "default" : settings.model;

  const auto write = [&](const json& q, const std::string& fixture, int rep, const Turn& turn) {
    json row = {
        {"question_id", q["id"]},
        {"fixture", fixture},
        {"backend", settings.backend},
        {"model", model_label},
        {"rep", rep},
        {"variant", settings.baseline ? "baseline" : "features"},
        {"reply", turn.reply},
        {"tool_calls", turn.tool_calls},
        {"input_tokens", turn.metrics.input_tokens},
        {"output_tokens", turn.metrics.output_tokens},
    };
    if (!turn.error.empty()) {
      row["error"] = turn.error;
    }
    out << row.dump() << "\n";
    out.flush();
    std::cerr << "[" << q["id"].get<std::string>() << " " << fixture << " rep " << rep << "] "
              << (turn.error.empty() ? "ok" : "error: " + turn.error) << ", " << turn.tool_calls.size()
              << " tool call(s)\n";
    EXPECT_TRUE(turn.error.empty()) << q["id"] << " " << fixture << ": " << turn.error;
  };

  int conversations = 0;
  for (const auto& q : questions) {
    if (q.contains("follows")) {
      continue;  // asked inside its parent's conversation
    }
    const auto& children = follow_ups[q["id"].get<std::string>()];
    const auto asks = [&](const json& child, const std::string& fixture) {
      const auto& fixtures = child["fixtures"];
      return std::find(fixtures.begin(), fixtures.end(), fixture) != fixtures.end();
    };
    for (const auto& fixture_name : q["fixtures"]) {
      const std::string fixture = fixture_name.get<std::string>();
      // The parent turn runs when it is selected itself, or as the setup of
      // a selected follow-up asked against this fixture.
      bool wanted = selected(q);
      for (const auto& child : children) {
        wanted = wanted || (selected(child) && asks(child, fixture));
      }
      if (!wanted) {
        continue;
      }
      for (int rep = 1; rep <= settings.repeat; ++rep) {
        auto memory = std::make_shared<HarnessMemory>();
        Backend backend(settings.cli, settings.model, memory);
        TurnTools tools;
        tools.registry = &registry;
        tools.invoke = [&](const std::string& n, const json& a) { return registry.execute(n, a, ctx); };
        tools.catalog = kEmptyCatalog;
        if (settings.baseline) {
          backend.setInstructions(assistant_agent::assistantInstructions(/*with_product_policy=*/false));
        } else {
          tools.features = assistant_agent::featuresDigest(
              assistant_agent::testing::readFileToString(bench + "/fixtures/" + fixture + ".json"));
        }

        const Turn first = runTurn(backend, q["text"].get<std::string>(), tools);
        if (selected(q)) {
          write(q, fixture, rep, first);
        }
        for (const auto& child : children) {
          if (!asks(child, fixture)) {
            continue;
          }
          const Turn next = runTurn(backend, child["text"].get<std::string>(), tools);
          if (selected(child)) {
            write(child, fixture, rep, next);
          }
        }
        ++conversations;
        if constexpr (requires { backend.rateLimited(); }) {
          if (backend.rateLimited()) {
            FAIL() << "usage limit reached after " << conversations << " conversation(s); partial results in "
                   << settings.out_path;
          }
        }
      }
    }
  }
  std::cerr << conversations << " conversation(s) written to " << settings.out_path << "\n";
  EXPECT_GT(conversations, 0) << "ASSISTANT_FEATURES_EVAL_ONLY matched no question";
}

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
TEST(FeaturesEval, AnswersTheProductQuestions) {
  const std::optional<std::string> backend = PJ::sdk::getEnv("ASSISTANT_FEATURES_EVAL");
  if (!backend) {
    GTEST_SKIP() << "set ASSISTANT_FEATURES_EVAL=claude|codex (needs a logged-in CLI; spends your subscription)";
  }
  Settings settings;
  settings.backend = *backend;
  const std::optional<std::string> model = PJ::sdk::getEnv("ASSISTANT_FEATURES_EVAL_MODEL");
  settings.out_path = PJ::sdk::getEnv("ASSISTANT_FEATURES_EVAL_OUT").value_or("features_eval.jsonl");
  settings.baseline = PJ::sdk::getEnv("ASSISTANT_FEATURES_EVAL_BASELINE").value_or("") == "1";
  settings.only = splitIds(PJ::sdk::getEnv("ASSISTANT_FEATURES_EVAL_ONLY").value_or(""));
  if (const auto repeat = PJ::sdk::getEnv("ASSISTANT_FEATURES_EVAL_REPEAT")) {
    settings.repeat = PJ::parseNumber<int>(*repeat).value_or(0);
    ASSERT_GE(settings.repeat, 1) << "ASSISTANT_FEATURES_EVAL_REPEAT must be a positive integer";
  }

  if (settings.backend == "claude") {
    settings.cli = PJ::sdk::getEnv("ASSISTANT_CLAUDE_CLI").value_or("claude");
    settings.model = model.value_or("sonnet");  // the panel's own default (assistant_dialog.cpp)
    runEval<assistant_agent::ClaudeBackend>(settings);
  } else if (settings.backend == "codex") {
    settings.cli = PJ::sdk::getEnv("ASSISTANT_CODEX_CLI").value_or("codex");
    settings.model = model.value_or("");
    runEval<assistant_agent::CodexBackend>(settings);
  } else {
    FAIL() << "ASSISTANT_FEATURES_EVAL must be claude or codex, got '" << settings.backend << "'";
  }
}
#endif

}  // namespace
