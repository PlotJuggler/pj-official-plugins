// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Drives McpHttpServer over loopback with an ix::HttpClient posing as the Claude
// MCP client: bearer auth, initialize, tools/list, and a tools/call that runs a
// real tool against a ToolboxTestStore.
#include <gtest/gtest.h>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>

#include <nlohmann/json.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>

#include "mcp_http_server.hpp"
#include "tool_registry.hpp"

namespace {

using assistant_agent::McpHttpServer;
using assistant_agent::ToolContext;
using assistant_agent::ToolRegistry;
using nlohmann::json;

// POST a JSON-RPC body with the given bearer token; returns (statusCode, parsed body).
std::pair<int, json> rpc(const std::string& url, const std::string& token, const json& body) {
  ix::initNetSystem();
  ix::HttpClient client(false);
  auto args = client.createRequest();
  args->extraHeaders["Content-Type"] = "application/json";
  args->extraHeaders["Authorization"] = "Bearer " + token;
  args->connectTimeout = 5;
  args->transferTimeout = 10;
  auto resp = client.post(url, body.dump(), args);
  if (resp == nullptr) {
    return {-1, json()};
  }
  return {resp->statusCode, json::parse(resp->body, nullptr, /*allow_exceptions=*/false)};
}

struct Fixture {
  ToolRegistry registry;
  PJ::testing::ToolboxTestStore store;
  ToolContext ctx;
  McpHttpServer server;

  Fixture() : server(registry, [this](const std::string& n, const json& a) { return registry.execute(n, a, ctx); }) {
    store.addTopic("/imu");
    store.addField("/imu", "x", {0}, {1.0});
    ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());
  }
};

TEST(McpServer, RejectsMissingToken) {
  Fixture fx;
  ASSERT_TRUE(fx.server.start());
  auto [status, body] = rpc(fx.server.url(), "wrong-token", {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
  EXPECT_EQ(status, 401);
}

TEST(McpServer, Initialize) {
  Fixture fx;
  ASSERT_TRUE(fx.server.start());
  auto [status, body] =
      rpc(fx.server.url(), fx.server.token(), {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
  EXPECT_EQ(status, 200);
  ASSERT_TRUE(body.contains("result"));
  EXPECT_TRUE(body["result"].contains("protocolVersion"));
  EXPECT_TRUE(body["result"]["capabilities"].contains("tools"));
}

TEST(McpServer, ToolsList) {
  Fixture fx;
  ASSERT_TRUE(fx.server.start());
  auto [status, body] =
      rpc(fx.server.url(), fx.server.token(), {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
  EXPECT_EQ(status, 200);
  ASSERT_TRUE(body.contains("result"));
  EXPECT_EQ(body["result"]["tools"].size(), 14u);
}

TEST(McpServer, ToolsCallRunsTool) {
  Fixture fx;
  ASSERT_TRUE(fx.server.start());
  auto [status, body] =
      rpc(fx.server.url(), fx.server.token(),
          {{"jsonrpc", "2.0"},
           {"id", 3},
           {"method", "tools/call"},
           {"params", {{"name", "list_topics"}, {"arguments", json::object()}}}});
  EXPECT_EQ(status, 200);
  ASSERT_TRUE(body.contains("result"));
  EXPECT_FALSE(body["result"].value("isError", true));
  const std::string text = body["result"]["content"][0]["text"].get<std::string>();
  EXPECT_NE(text.find("/imu"), std::string::npos);
}

TEST(McpServer, ToolsCallErrorFlagsIsError) {
  Fixture fx;
  ASSERT_TRUE(fx.server.start());
  auto [status, body] =
      rpc(fx.server.url(), fx.server.token(),
          {{"jsonrpc", "2.0"},
           {"id", 4},
           {"method", "tools/call"},
           {"params", {{"name", "describe_topic"}, {"arguments", {{"topic", "/nope"}}}}}});
  EXPECT_EQ(status, 200);
  ASSERT_TRUE(body.contains("result"));
  EXPECT_TRUE(body["result"].value("isError", false));
}

TEST(McpServer, UnknownMethodIsRpcError) {
  Fixture fx;
  ASSERT_TRUE(fx.server.start());
  auto [status, body] =
      rpc(fx.server.url(), fx.server.token(), {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "does/not/exist"}});
  EXPECT_EQ(status, 200);
  EXPECT_TRUE(body.contains("error"));
}

TEST(McpServer, McpConfigJsonWellFormed) {
  Fixture fx;
  ASSERT_TRUE(fx.server.start());
  auto cfg = json::parse(fx.server.mcpConfigJson(), nullptr, false);
  ASSERT_TRUE(cfg.is_object());
  const auto& pj = cfg["mcpServers"]["pj"];
  EXPECT_EQ(pj["type"], "http");
  EXPECT_EQ(pj["url"], fx.server.url());
  EXPECT_EQ(pj["headers"]["Authorization"], "Bearer " + fx.server.token());
}

}  // namespace
