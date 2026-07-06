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
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <vector>

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

}  // namespace
