// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/number_parse.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "llm_backend.hpp"

namespace assistant_agent {

// A scripted, LLM-free backend that maps a few English phrases to real tool
// calls. It exists so the whole tool + GuiExecutor path can be driven
// deterministically — in unit tests and, via ASSISTANT_FAKE_BACKEND=1, live in
// PJ4 — before any network/subprocess backend exists. It is NOT a parser of
// natural language; it recognises a handful of command shapes:
//
//   "list topics"                          -> list_topics
//   "status"                               -> report_status
//   "describe <topic>"                     -> describe_topic
//   "stats <topic/field>"                  -> read_series (stats)
//   "derivative of <topic/field>"          -> create_derived_series (stateful)
//   "mark <topic/field> > <n>"             -> create_markers
//   "crop <topic> at <t>"                  -> evaluate (object path, at_s)
//   "pin <topic> at <t>"                   -> create_derived_object (pin_at_s)
//   "show <topic> in 3d"                   -> scene_view (create + attach)
//
// Anything else echoes usage. Every tool call blocks on tools.invoke (the
// GuiExecutor), so this exercises the exact cross-thread path a real backend
// uses.
class FakeBackend : public LlmBackend {
 public:
  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override {
    cancelled_.store(false);
    const std::string lower = PJ::sdk::lowerAscii(text);
    const std::vector<std::string> words = split(text);

    auto run = [&](const std::string& tool, const nlohmann::json& args) {
      sink({BackendEvent::Kind::ToolActivity, tool + "(" + args.dump() + ")"});
      if (!tools.invoke) {
        sink({BackendEvent::Kind::Error, "no tool invoker wired"});
        return;
      }
      ToolResult r = tools.invoke(tool, args);
      sink({BackendEvent::Kind::AssistantText, (r.ok ? "" : "(error) ") + r.content});
    };

    if (contains(lower, "list") || contains(lower, "topics")) {
      run("list_topics", nlohmann::json::object());
    } else if (contains(lower, "status")) {
      run("report_status", nlohmann::json::object());
    } else if (contains(lower, "derivative")) {
      const std::string path = firstPath(words);
      if (path.empty()) {
        sink({BackendEvent::Kind::AssistantText, "Say: derivative of <topic/field>"});
      } else {
        run("create_derived_series", {{"name", leaf(path) + "_derivative"},
                                      {"inputs", nlohmann::json::array({path})},
                                      {"global", "  local pt, pv = nil, nil"},
                                      {"body",
                                       "    if pt == nil then pt, pv = time, value; return end\n"
                                       "    local dt = time - pt\n"
                                       "    local d = dt ~= 0 and (value - pv) / dt or 0\n"
                                       "    pt, pv = time, value\n"
                                       "    return d"}});
      }
    } else if (startsWith(lower, "describe")) {
      if (words.size() >= 2) {
        run("describe_topic", {{"topic", words[1]}});
      } else {
        sink({BackendEvent::Kind::AssistantText, "Say: describe <topic>"});
      }
    } else if (startsWith(lower, "stats") || startsWith(lower, "read")) {
      const std::string path = firstPath(words);
      if (path.empty()) {
        sink({BackendEvent::Kind::AssistantText, "Say: stats <topic/field>"});
      } else {
        run("read_series", {{"series", path}, {"mode", "stats"}});
      }
    } else if (startsWith(lower, "mark")) {
      const std::string path = firstPath(words);
      double threshold = 0.0;
      std::string cmp = ">";
      for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        if (words[i] == ">" || words[i] == "<" || words[i] == ">=" || words[i] == "<=") {
          cmp = words[i];
          threshold = parseDouble(words[i + 1]);
        }
      }
      if (path.empty()) {
        sink({BackendEvent::Kind::AssistantText, "Say: mark <topic/field> > <threshold>"});
      } else {
        run("create_markers", {{"series", path}, {"comparison", cmp}, {"threshold", threshold}});
      }
    } else if (startsWith(lower, "crop")) {
      const std::string path = firstPath(words);
      const auto at_s = findAt(words);
      if (path.empty() || !at_s) {
        sink({BackendEvent::Kind::AssistantText, "Say: crop <topic> at <t>"});
      } else {
        // The crop_box body/outputs shape object_ops_catalog.hpp advertises
        // for kPointCloud: a cropped cloud plus its point count, so the
        // result reads as more than just "created".
        run("evaluate", {{"inputs", nlohmann::json::array({path})},
                         {"at_s", *at_s},
                         {"body", "  local c = inputs[\"" + path +
                                      "\"]:crop_box{min={-1,-1,-1},max={1,1,1}}\n"
                                      "  return { cropped = c, count = c:count() }"},
                         {"outputs", nlohmann::json::array({"cropped:kPointCloud", "count:number"})}});
      }
    } else if (startsWith(lower, "pin")) {
      const std::string path = firstPath(words);
      const auto at_s = findAt(words);
      if (path.empty() || !at_s) {
        sink({BackendEvent::Kind::AssistantText, "Say: pin <topic> at <t>"});
      } else {
        run("create_derived_object", {{"name", leaf(path) + "_pin"},
                                      {"inputs", nlohmann::json::array({path})},
                                      {"outputs", nlohmann::json::array({"cropped:kPointCloud", "count:number"})},
                                      {"body", "  local c = inputs[\"" + path +
                                                   "\"]:crop_box{min={-1,-1,-1},max={1,1,1}}\n"
                                                   "  return { cropped = c, count = c:count() }"},
                                      {"pin_at_s", *at_s}});
      }
    } else if (startsWith(lower, "show")) {
      const std::string path = firstPath(words);
      if (path.empty()) {
        sink({BackendEvent::Kind::AssistantText, "Say: show <topic> in 3d"});
      } else {
        // Two tool calls, same as a model would issue them: an empty view of
        // its own, then the topic attached to it.
        run("scene_view", {{"action", "create"}, {"view", "scene"}, {"kind", "3d"}});
        run("scene_view", {{"action", "attach"}, {"view", "scene"}, {"topics", nlohmann::json::array({path})}});
      }
    } else {
      sink(
          {BackendEvent::Kind::AssistantText,
           "FakeBackend commands: 'list topics', 'status', 'describe <topic>', 'stats <topic/field>', "
           "'derivative of <topic/field>', 'mark <topic/field> > <n>', 'crop <topic> at <t>', "
           "'pin <topic> at <t>', 'show <topic> in 3d'."});
    }
    sink({BackendEvent::Kind::TurnComplete, {}});
  }

  void cancel() override {
    cancelled_.store(true);
  }

  [[nodiscard]] std::string name() const override {
    return "Fake (scripted, no LLM)";
  }

 private:
  static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  }
  static bool startsWith(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
  }
  static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) {
      out.push_back(w);
    }
    return out;
  }
  // First token that looks like a curve path (has a '/').
  static std::string firstPath(const std::vector<std::string>& words) {
    for (const auto& w : words) {
      if (w.find('/') != std::string::npos) {
        return w;
      }
    }
    return {};
  }
  // The number following a literal "at" token ("crop <topic> at <t>"),
  // display seconds. Empty when there is no "at" or nothing parses after it.
  static std::optional<double> findAt(const std::vector<std::string>& words) {
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
      if (PJ::sdk::lowerAscii(words[i]) == "at") {
        return PJ::parseNumber<double>(words[i + 1]);
      }
    }
    return std::nullopt;
  }
  static std::string leaf(const std::string& path) {
    const auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
  }
  static double parseDouble(const std::string& s) {
    // PJ::parseNumber, not std::stod: it is a whole-string parse that reports
    // failure instead of throwing, and it does not follow LC_NUMERIC (std::stod
    // mis-reads "1.5" under a comma-decimal locale).
    return PJ::parseNumber<double>(s).value_or(0.0);
  }

  std::atomic<bool> cancelled_{false};
};

}  // namespace assistant_agent
