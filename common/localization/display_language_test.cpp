// Copyright 2026 PlotJuggler Chinese localization contributors
// SPDX-License-Identifier: MIT
#include <display_language.hpp>
#include <cctype>
#include <clocale>
#include <iostream>
#include <stdexcept>
#include <string>
// This focused path/prose fixture reads no compressed MCAP data. Production
// plugin and upstream tests keep their normal LZ4/Zstd build configuration.
#ifdef PJ_TEST_MCAP
#define MCAP_IMPLEMENTATION
#define MCAP_COMPRESSION_NO_LZ4
#define MCAP_COMPRESSION_NO_ZSTD
#include "data_load_mcap/mcap_helpers.hpp"
#endif
#ifdef PJ_TEST_ASSISTANT
#include "toolbox_assistant_agent/src/chat_session.hpp"
#endif

static void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  require(argc == 2, "expected language argument");
  using pj::localization::Language;
  const std::string requested = argv[1];
  const Language expected = requested == "zh_CN" ? Language::SimplifiedChinese :
      requested == "zh_TW" ? Language::TraditionalChinese : Language::English;
  const bool chinese = expected != Language::English;
  // The expected value is independent of the helper under test.
  const auto expectedText = [expected](const char* english, const char* simplified,
                                       const char* traditional) {
    return expected == Language::SimplifiedChinese ? simplified :
        expected == Language::TraditionalChinese ? traditional : english;
  };
  const std::string numeric_locale = std::setlocale(LC_NUMERIC, nullptr);
  require(pj::localization::language() == expected, "startup language ID mismatch");
  require(pj::localization::isChinese() == chinese, "startup language mismatch");
  require(std::string(pj::localization::text("Ready", "就绪", "就緒")) ==
              expectedText("Ready", "就绪", "就緒"), "fixed prose mismatch");
#ifdef PJ_TEST_MCAP
  const std::string missing_path = "__pj_nonexistent_raw_%1_Parser Error_汉漢_zh_TW.yaml";
  const auto recording = PJ::McapHelpers::resolveRecordingFiles(missing_path);
  require(recording.paths.empty(), "invalid recording unexpectedly resolved");
  require(recording.error == std::string(expectedText("Cannot open ", "无法打开 ", "無法開啟 ")) + missing_path,
          "MCAP fixed prose or raw path changed");
  const std::string raw_mcap = "raw_%1_Parser Error_汉漢_zh_TW.mcap";
  const auto plain = PJ::McapHelpers::resolveRecordingFiles(raw_mcap);
  require(plain.error.empty() && plain.paths == std::vector<std::string>{raw_mcap},
          "MCAP canonical path changed");
#endif
  const std::string raw = "**Assistant:**\nError: %1\nSOURCE NOTICE:\n用户数据_使用者資料_zh_CN_zh_TW ```cpp int x=1;```";
#ifdef PJ_TEST_ASSISTANT
  assistant_agent::ChatSession session;
  session.addUser(raw);
  session.addAssistant(raw);
  session.addSystem("Error: " + raw,
      std::string(pj::localization::text("Error: ", "错误：", "錯誤：")) + raw);
  session.addTool(raw);
  const auto rendered = session.renderMarkdown();
  require(rendered.find(chinese ? "**你：**" : "**You:**") != std::string::npos,
          "user role must use display language");
  require(rendered.find(chinese ? "**助手：**" : "**Assistant:**") != std::string::npos,
          "assistant role must use display language");
  require(session.messages()[0].text == raw && session.messages()[1].text == raw,
          "raw user/model content changed");
  require(session.messages()[2].text == "Error: " + raw, "canonical system content changed");
  require(session.messages()[3].text == raw, "raw tool content changed");
  require(session.statusText() == expectedText("Ready", "就绪", "就緒"),
          "assistant status must use display language");
  session.setState(assistant_agent::TurnState::WaitingForLlm);
  require(session.statusText() == expectedText("Thinking…", "正在思考…", "正在思考…"),
          "assistant waiting status must use display language");
  session.setState(assistant_agent::TurnState::ExecutingTool);
  require(session.statusText() == expectedText("Running a tool…", "正在运行工具…", "正在執行工具…"),
          "assistant tool status must use display language");
  session.setState(assistant_agent::TurnState::Idle);
  require(rendered.find(expectedText("**System:**", "**系统：**", "**系統：**")) != std::string::npos,
          "system role must use display language");
  require(rendered.find(expectedText("**Tool:**", "**工具：**", "**工具：**")) != std::string::npos,
          "tool role must use display language");
  std::size_t position = 0;
  int occurrences = 0;
  while ((position = rendered.find(raw, position)) != std::string::npos) {
    ++occurrences;
    position += raw.size();
  }
  require(occurrences == 2, "display formatting changed embedded user/model content");
  const std::string system_body = std::string(expectedText("Error: ", "错误：", "錯誤：")) + raw;
  std::string escaped;
  for (char ch : system_body) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 128 && std::ispunct(byte) != 0) escaped.push_back('\\');
    escaped.push_back(ch);
  }
  require(rendered.find(escaped) != std::string::npos,
          "system display override or existing Markdown escaping lost");
#else
  require(std::string(pj::localization::text("Error: ", "错误：", "錯誤：")) + raw ==
              expectedText("Error: ", "错误：", "錯誤：") + raw, "raw payload changed");
#endif
  require(numeric_locale == std::setlocale(LC_NUMERIC, nullptr),
          "display language changed numeric parsing locale");
  // A preference update cannot mutate the startup snapshot. This fixture is
  // single-threaded; production only writes the environment before workers.
  const char* changed = expected == Language::TraditionalChinese ? "en_US" : "zh_TW";
#ifdef _WIN32
  require(_putenv_s("PLOTJUGGLER_UI_LANGUAGE", changed) == 0, "cannot update test environment");
#else
  require(setenv("PLOTJUGGLER_UI_LANGUAGE", changed, 1) == 0, "cannot update test environment");
#endif
  require(pj::localization::language() == expected, "startup language snapshot changed");
  require(std::string(pj::localization::text("Ready", "就绪", "就緒")) ==
              expectedText("Ready", "就绪", "就緒"), "display changed without restart");
  std::cout << "offline display and canonical-data checks passed\n";
}
