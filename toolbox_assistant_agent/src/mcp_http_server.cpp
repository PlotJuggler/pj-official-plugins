// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "mcp_http_server.hpp"

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>

#include <random>
#include <utility>

namespace assistant_agent {
namespace {

using nlohmann::json;

// MCP protocol revision this server implements. Claude negotiates against it.
constexpr const char* kProtocolVersion = "2025-06-18";

// Loopback port window scanned for a free bind (ephemeral port 0 is not
// supported by the vendored ixwebsocket build).
constexpr int kPortLo = 39600;
constexpr int kPortHi = 39680;

std::string randomToken() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  static const char* hex = "0123456789abcdef";
  std::string t;
  t.reserve(32);
  for (int i = 0; i < 32; ++i) {
    t.push_back(hex[dist(rd)]);
  }
  return t;
}

json rpcError(const json& id, int code, const std::string& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json rpcResult(const json& id, json result) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

ix::HttpResponsePtr jsonResponse(int status, const std::string& body) {
  return std::make_shared<ix::HttpResponse>(
      status, status == 200 ? "OK" : "Error", ix::HttpErrorCode::Ok,
      ix::WebSocketHttpHeaders{{"Content-Type", "application/json"}}, body);
}

}  // namespace

McpHttpServer::McpHttpServer(const ToolRegistry& registry, ToolInvoker invoker)
    : registry_(registry), invoker_(std::move(invoker)), token_(randomToken()) {}

McpHttpServer::~McpHttpServer() {
  stop();
}

std::string McpHttpServer::mcpConfigJson() const {
  json cfg = {
      {"mcpServers",
       {{"pj", {{"type", "http"}, {"url", url()}, {"headers", {{"Authorization", "Bearer " + token_}}}}}}}};
  return cfg.dump();
}

bool McpHttpServer::start() {
  ix::initNetSystem();
  for (int port = kPortLo; port < kPortHi; ++port) {
    auto server = std::make_unique<ix::HttpServer>(port, "127.0.0.1");
    server->setOnConnectionCallback(
        [this](ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
          // Auth: constant "Bearer <token>" match. A missing/wrong token is 401.
          auto it = req->headers.find("Authorization");
          const std::string expected = "Bearer " + token_;
          if (it == req->headers.end() || it->second != expected) {
            return jsonResponse(401, R"({"error":"unauthorized"})");
          }
          // We only serve POST JSON-RPC; a GET (server->client SSE channel) is
          // not offered — our tool replies are immediate.
          if (req->method != "POST") {
            return jsonResponse(405, R"({"error":"method not allowed"})");
          }
          auto body = json::parse(req->body, nullptr, /*allow_exceptions=*/false);
          if (body.is_discarded()) {
            return jsonResponse(400, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error"}})");
          }
          // JSON-RPC batch (array) or single request.
          if (body.is_array()) {
            json out = json::array();
            for (const auto& one : body) {
              json resp = handleRpc(one);
              if (!resp.is_null()) {
                out.push_back(std::move(resp));
              }
            }
            return jsonResponse(200, out.empty() ? std::string("") : out.dump());
          }
          json resp = handleRpc(body);
          return jsonResponse(200, resp.is_null() ? std::string("") : resp.dump());
        });
    auto [ok, err] = server->listen();
    if (!ok) {
      continue;  // port busy — try the next
    }
    server->start();
    server_ = std::move(server);
    port_ = port;
    return true;
  }
  return false;
}

void McpHttpServer::stop() {
  if (server_) {
    server_->stop();
    server_.reset();
  }
  port_ = 0;
}

nlohmann::json McpHttpServer::handleRpc(const nlohmann::json& req) {
  // Client JSON is untrusted and the typed getters below throw on wrong-typed
  // fields; this runs on the HTTP server thread where an escaped exception
  // would terminate the process, so answer with a JSON-RPC error instead.
  try {
    return handleRpcChecked(req);
  } catch (const std::exception& e) {
    return rpcError(nullptr, -32600, std::string("malformed request: ") + e.what());
  }
}

nlohmann::json McpHttpServer::handleRpcChecked(const nlohmann::json& req) {
  if (!req.is_object()) {
    return rpcError(nullptr, -32600, "invalid request");
  }
  const std::string method = req.value("method", std::string{});
  const bool is_notification = !req.contains("id");
  const json id = req.value("id", json(nullptr));

  if (method == "initialize") {
    return rpcResult(
        id, {{"protocolVersion", kProtocolVersion},
             {"capabilities", {{"tools", json::object()}}},
             {"serverInfo", {{"name", "plotjuggler-assistant"}, {"version", "0.1.0"}}}});
  }
  if (method == "tools/list") {
    return rpcResult(id, {{"tools", registry_.toMcpToolsList()}});
  }
  if (method == "tools/call") {
    const json params = req.value("params", json::object());
    const std::string name = params.value("name", std::string{});
    const json args = params.value("arguments", json::object());
    ToolResult result = invoker_ ? invoker_(name, args) : ToolResult::failure("no tool invoker wired");
    return rpcResult(
        id, {{"content", json::array({{{"type", "text"}, {"text", result.content}}})}, {"isError", !result.ok}});
  }

  // notifications/initialized and other notifications need no reply.
  if (is_notification) {
    return json(nullptr);
  }
  return rpcError(id, -32601, "method not found: " + method);
}

}  // namespace assistant_agent
