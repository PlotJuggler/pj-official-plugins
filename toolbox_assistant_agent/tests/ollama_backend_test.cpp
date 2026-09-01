// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Drives OllamaBackend against an in-process ix::HttpServer impersonating
// Ollama: /api/tags for the connectivity probe and a two-round /api/chat that
// first asks for a tool call, then answers. The tool actually runs against a
// ToolboxTestStore, so this exercises the whole loop: request -> tool_calls ->
// tool result fed back -> final answer.
#include "ollama_backend.hpp"

#include <gtest/gtest.h>
#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/settings_store_host.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <vector>

#include "conversation_state.hpp"
#include "settings_store.hpp"
#include "tool_registry.hpp"

namespace {

using assistant_agent::BackendEvent;
using assistant_agent::OllamaBackend;
using assistant_agent::ToolContext;
using assistant_agent::ToolRegistry;
using assistant_agent::TurnTools;

// Minimal fake Ollama HTTP server on a concrete loopback port. This ixwebsocket
// build does not bind port 0 (ephemeral), so we scan a small range until one
// port listens.
class FakeOllama {
 public:
  explicit FakeOllama(int port) : server_(port, "127.0.0.1") {
    ix::initNetSystem();
    server_.setOnConnectionCallback(
        [this](ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState>) { return handle(req); });
    auto [ok, err] = server_.listen();
    listen_ok_ = ok;
    listen_err_ = err;
    if (listen_ok_) {
      server_.start();
      port_ = port;
    }
  }
  ~FakeOllama() {
    server_.stop();
  }

  [[nodiscard]] bool ready() const {
    return listen_ok_ && port_ > 0;
  }
  [[nodiscard]] std::string listenError() const {
    return listen_err_;
  }
  [[nodiscard]] int port() const {
    return port_;
  }
  [[nodiscard]] std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }
  [[nodiscard]] int chatCalls() const {
    return chat_calls_.load();
  }
  // Body of the most recent /api/chat request — this is where the conversation
  // the backend believes it is having becomes observable.
  [[nodiscard]] std::string lastChatBody() const {
    std::lock_guard<std::mutex> lk(body_mu_);
    return last_chat_body_;
  }

 private:
  ix::HttpResponsePtr json(const std::string& body) {
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, ix::WebSocketHttpHeaders{{"Content-Type", "application/json"}}, body);
  }

  ix::HttpResponsePtr handle(ix::HttpRequestPtr req) {
    if (req->uri.find("/api/tags") != std::string::npos) {
      return json(R"({"models":[{"name":"test-model"}]})");
    }
    if (req->uri.find("/api/chat") != std::string::npos) {
      {
        std::lock_guard<std::mutex> lk(body_mu_);
        last_chat_body_ = req->body;
      }
      const int n = ++chat_calls_;
      if (n == 1) {
        // First round: ask the model to call list_topics.
        return json(
            R"({"message":{"role":"assistant","content":"",)"
            R"("tool_calls":[{"function":{"name":"list_topics","arguments":{}}}]},"done":true})");
      }
      // Second round: the model has the tool result; answer.
      return json(R"({"message":{"role":"assistant","content":"You have one topic."},"done":true})");
    }
    return json("{}");
  }

  ix::HttpServer server_;
  int port_ = 0;
  bool listen_ok_ = false;
  std::string listen_err_;
  std::atomic<int> chat_calls_{0};
  mutable std::mutex body_mu_;
  std::string last_chat_body_;
};

// Bind the first free port in a small range, so parallel/repeat test runs don't
// collide. Returns nullptr if the whole range is busy.
std::unique_ptr<FakeOllama> startFakeOllama() {
  for (int port = 39500; port < 39560; ++port) {
    auto server = std::make_unique<FakeOllama>(port);
    if (server->ready()) {
      return server;
    }
  }
  return nullptr;
}

// Collects all events a turn emits (backend runs synchronously on this thread).
std::vector<BackendEvent> runTurn(OllamaBackend& backend, const std::string& text, const TurnTools& tools) {
  std::vector<BackendEvent> events;
  std::mutex mu;
  backend.sendUserMessage(text, tools, [&](BackendEvent e) {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(std::move(e));
  });
  return events;
}

bool has(const std::vector<BackendEvent>& evs, BackendEvent::Kind kind, const std::string& needle) {
  for (const auto& e : evs) {
    if (e.kind == kind && e.text.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TEST(OllamaBackend, TestConnectionReportsModels) {
  auto server = startFakeOllama();
  ASSERT_NE(server, nullptr) << "no free port to bind the fake Ollama server";
  OllamaBackend backend(server->url(), "test-model");
  auto r = backend.testConnection();
  EXPECT_TRUE(r.ok) << r.message;
  EXPECT_NE(r.message.find("model"), std::string::npos);
}

TEST(OllamaBackend, TestConnectionUnreachable) {
  OllamaBackend backend("http://127.0.0.1:9", "m");  // nothing listening on :9
  auto r = backend.testConnection();
  EXPECT_FALSE(r.ok);
}

TEST(OllamaBackend, EmptyModelErrorsWithoutHttp) {
  OllamaBackend backend("http://127.0.0.1:9", "");
  ToolRegistry reg;
  TurnTools tools;
  tools.registry = &reg;
  auto evs = runTurn(backend, "hi", tools);
  EXPECT_TRUE(has(evs, BackendEvent::Kind::Error, "model"));
  EXPECT_TRUE(has(evs, BackendEvent::Kind::TurnComplete, ""));
}

TEST(OllamaBackend, ToolCallRoundTrip) {
  auto server = startFakeOllama();
  ASSERT_NE(server, nullptr) << "no free port to bind the fake Ollama server";
  OllamaBackend backend(server->url(), "test-model");

  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/imu");
  store.addField("/imu", "x", {0}, {1.0});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());

  TurnTools tools;
  tools.registry = &reg;
  tools.invoke = [&](const std::string& n, const nlohmann::json& a) { return reg.execute(n, a, ctx); };

  auto evs = runTurn(backend, "list my topics", tools);

  EXPECT_EQ(server->chatCalls(), 2);  // tool round + answer round
  EXPECT_TRUE(has(evs, BackendEvent::Kind::ToolActivity, "list_topics"));
  EXPECT_TRUE(has(evs, BackendEvent::Kind::AssistantText, "one topic"));
  EXPECT_TRUE(has(evs, BackendEvent::Kind::TurnComplete, ""));
}

// The conversation must survive the backend object. Saving any setting rebuilds
// the backend, and before the memory was hoisted out of it that silently
// restarted the conversation: the user changed a model and the assistant lost
// everything it had been told, with nothing on screen to say so.
TEST(OllamaBackend, ConversationSurvivesARebuild) {
  auto server = startFakeOllama();
  ASSERT_NE(server, nullptr) << "no free port to bind the fake Ollama server";

  auto memory = std::make_shared<assistant_agent::OllamaMemory>();
  ToolRegistry reg;
  TurnTools tools;
  tools.registry = &reg;
  tools.invoke = [&](const std::string&, const nlohmann::json&) { return assistant_agent::ToolResult::success("{}"); };

  {
    OllamaBackend first(server->url(), "test-model", memory);
    runTurn(first, "remember the number 41", tools);
  }  // the settings modal is accepted here: `first` is destroyed

  OllamaBackend second(server->url(), "test-model", memory);
  runTurn(second, "what number did I say?", tools);

  // What the rebuilt backend actually put on the wire carries the earlier turn.
  const std::string body = server->lastChatBody();
  EXPECT_NE(body.find("remember the number 41"), std::string::npos)
      << "the rebuilt backend forgot the conversation; body was: " << body;
  EXPECT_NE(body.find("what number did I say?"), std::string::npos);
}

// The conversation must also survive the PROCESS: the dialog saves the memory's
// history into the settings store after every turn and restores it into a fresh
// memory on the next launch. Driven through the real serialize/deserialize path
// (ConversationState over an in-memory settings host), then proven on the wire.
TEST(OllamaBackend, ConversationSurvivesAProcessRestart) {
  auto server = startFakeOllama();
  ASSERT_NE(server, nullptr) << "no free port to bind the fake Ollama server";

  ToolRegistry reg;
  TurnTools tools;
  tools.registry = &reg;
  tools.invoke = [&](const std::string&, const nlohmann::json&) { return assistant_agent::ToolResult::success("{}"); };

  PJ::sdk::InMemorySettingsBackend settings_backend;
  PJ::sdk::SettingsStoreHost settings_host{settings_backend};

  {
    // "First process": run a turn, then persist what the dialog would persist.
    auto memory = std::make_shared<assistant_agent::OllamaMemory>();
    OllamaBackend first(server->url(), "test-model", memory);
    runTurn(first, "remember the number 41", tools);

    assistant_agent::ConversationState conv;
    conv.ollama_history_json = memory->history.dump();
    assistant_agent::SettingsStore store(PJ::sdk::SettingsView{settings_host.view()});
    assistant_agent::saveConversation(store, conv);
  }

  // "Second process": a fresh memory restored from the store, a fresh backend.
  const assistant_agent::ConversationState back =
      assistant_agent::loadConversation(assistant_agent::SettingsStore(PJ::sdk::SettingsView{settings_host.view()}));
  ASSERT_FALSE(back.ollama_history_json.empty());
  auto restored = std::make_shared<assistant_agent::OllamaMemory>();
  restored->history = nlohmann::json::parse(back.ollama_history_json);

  OllamaBackend second(server->url(), "test-model", restored);
  runTurn(second, "what number did I say?", tools);

  const std::string body = server->lastChatBody();
  EXPECT_NE(body.find("remember the number 41"), std::string::npos)
      << "the restored conversation is not on the wire; body was: " << body;
}

// The other half of the contract: a backend handed no memory keeps its own, so
// the tests above (and any future caller that wants a clean slate) are not
// quietly sharing state.
TEST(OllamaBackend, PrivateMemoryWhenNoneIsLent) {
  auto server = startFakeOllama();
  ASSERT_NE(server, nullptr) << "no free port to bind the fake Ollama server";

  ToolRegistry reg;
  TurnTools tools;
  tools.registry = &reg;
  tools.invoke = [&](const std::string&, const nlohmann::json&) { return assistant_agent::ToolResult::success("{}"); };

  {
    OllamaBackend first(server->url(), "test-model");
    runTurn(first, "remember the number 41", tools);
  }
  OllamaBackend second(server->url(), "test-model");
  runTurn(second, "what number did I say?", tools);

  EXPECT_EQ(server->lastChatBody().find("remember the number 41"), std::string::npos);
}

}  // namespace
