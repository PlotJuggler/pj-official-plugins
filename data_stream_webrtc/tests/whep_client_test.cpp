// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// whep_client tests. Pure URL helpers are tested directly; the HTTP functions
// are tested against a local ix::HttpServer stub (loopback only — the single
// deliberate exception to the no-network-in-tests policy).
#include "whep_client.hpp"

#include <gtest/gtest.h>
#include <ixwebsocket/IXHttpServer.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace PJ {
namespace webrtc {
namespace {

TEST(WhepUrl, BuildJoinsWithSingleSlashes) {
  EXPECT_EQ(buildWhepUrl("http://h:8889", "cam0"), "http://h:8889/cam0/whep");
  EXPECT_EQ(buildWhepUrl("http://h:8889/", "cam0"), "http://h:8889/cam0/whep");
  EXPECT_EQ(buildWhepUrl("http://h:8889", "/cam0"), "http://h:8889/cam0/whep");
  // mediamtx paths may contain '/'
  EXPECT_EQ(buildWhepUrl("https://h", "site/cams/front"), "https://h/site/cams/front/whep");
}

TEST(WhepUrl, ResolveLocationAbsolutePassthrough) {
  EXPECT_EQ(resolveLocation("http://h:8889/cam0/whep", "http://other:1234/s/1"), "http://other:1234/s/1");
}

TEST(WhepUrl, ResolveLocationHostRelative) {
  // mediamtx answers with a host-relative Location like /cam0/whep/<uuid>
  EXPECT_EQ(resolveLocation("http://h:8889/cam0/whep", "/cam0/whep/abc-123"), "http://h:8889/cam0/whep/abc-123");
  EXPECT_EQ(resolveLocation("https://h/cam0/whep", "/x"), "https://h/x");
}

TEST(WhepUrl, ResolveLocationPathRelative) {
  EXPECT_EQ(resolveLocation("http://h:8889/cam0/whep", "abc-123"), "http://h:8889/cam0/abc-123");
}

// Records every request; replies with a configurable canned response.
// Loopback-only.
class StubWhepServer {
 public:
  struct Seen {
    std::string method;
    std::string uri;
    std::string body;
    std::string authorization;
    std::string content_type;
  };

  struct Reply {
    int status = 200;
    ix::WebSocketHttpHeaders headers;
    std::string body;
  };

  // ixwebsocket 11.4.6's SocketServer::getPort() just echoes back the ctor
  // argument (no getsockname() to resolve an OS-assigned ephemeral port), so
  // port 0 would make baseUrl() point at ":0". Use a fixed high port instead;
  // SO_REUSEADDR + synchronous stop() in the dtor make same-port reuse across
  // tests in this single-process, sequential gtest binary safe.
  StubWhepServer() : server_(kFixedPort, "127.0.0.1") {
    server_.setOnConnectionCallback(
        [this](ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
          std::lock_guard<std::mutex> lk(mutex_);
          Seen s;
          s.method = req->method;
          s.uri = req->uri;
          s.body = req->body;
          auto it = req->headers.find("Authorization");
          s.authorization = (it != req->headers.end()) ? it->second : "";
          it = req->headers.find("Content-Type");
          s.content_type = (it != req->headers.end()) ? it->second : "";
          seen_.push_back(std::move(s));
          Reply reply;  // default 200/{}/"" when nothing was scripted
          if (!replies_.empty()) {
            reply = replies_[std::min(next_reply_, replies_.size() - 1)];
            ++next_reply_;
          }
          return std::make_shared<ix::HttpResponse>(
              reply.status, "status", ix::HttpErrorCode::Ok, reply.headers, reply.body);
        });
    auto res = server_.listen();
    listening_ = res.first;
    server_.start();
  }
  ~StubWhepServer() {
    server_.stop();
  }

  bool listening() const {
    return listening_;
  }
  std::string baseUrl() const {
    return "http://127.0.0.1:" + std::to_string(kFixedPort);
  }
  void setReply(int status, ix::WebSocketHttpHeaders headers, std::string body) {
    setReplies({Reply{status, std::move(headers), std::move(body)}});
  }
  // Scripted mode: replies are consumed in request order; once the script is
  // exhausted, the LAST entry keeps being served (so setReply == a 1-entry script).
  void setReplies(std::vector<Reply> replies) {
    std::lock_guard<std::mutex> lk(mutex_);
    replies_ = std::move(replies);
    next_reply_ = 0;
  }
  std::vector<Seen> seen() {
    std::lock_guard<std::mutex> lk(mutex_);
    return seen_;
  }

 private:
  static constexpr int kFixedPort = 18889;

  ix::HttpServer server_;
  bool listening_ = false;
  std::mutex mutex_;  // guards replies_, next_reply_ and seen_ (test thread vs server thread)
  std::vector<Reply> replies_;
  std::size_t next_reply_ = 0;
  std::vector<Seen> seen_;
};

TEST(WhepPost, HappyPath201RelativeLocation) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  ix::WebSocketHttpHeaders h;
  h["Location"] = "/cam0/whep/session-1";
  stub.setReply(201, h, "v=0\r\nANSWER");

  const std::string url = stub.baseUrl() + "/cam0/whep";
  auto out = postOffer(url, "tok123", "v=0\r\nOFFER", std::chrono::seconds(5));
  ASSERT_TRUE(out);
  EXPECT_EQ(out->answer_sdp, "v=0\r\nANSWER");
  EXPECT_EQ(out->session_url, stub.baseUrl() + "/cam0/whep/session-1");

  auto seen = stub.seen();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].method, "POST");
  EXPECT_EQ(seen[0].uri, "/cam0/whep");
  EXPECT_EQ(seen[0].body, "v=0\r\nOFFER");
  EXPECT_EQ(seen[0].authorization, "Bearer tok123");
  EXPECT_EQ(seen[0].content_type, "application/sdp");
}

TEST(WhepPost, NoTokenMeansNoAuthorizationHeader) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  ix::WebSocketHttpHeaders h;
  h["Location"] = "/s/1";
  stub.setReply(201, h, "ANSWER");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "", "OFFER", std::chrono::seconds(5));
  ASSERT_TRUE(out);
  auto seen = stub.seen();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].authorization, "");
}

TEST(WhepPost, Unauthorized401IsTyped) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(401, {}, "");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "bad", "OFFER", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kUnauthorized);
}

TEST(WhepPost, NotFound404IsTyped) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(404, {}, "path not ready");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "", "OFFER", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kNotFound);
}

TEST(WhepPost, MissingLocationIsBadResponse) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(201, {}, "ANSWER");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "", "OFFER", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kBadResponse);
}

TEST(WhepPost, ConnectionRefusedIsNetwork) {
  // Port 9 (discard) on loopback is closed; connect fails fast.
  auto out = postOffer("http://127.0.0.1:9/cam0/whep", "", "OFFER", std::chrono::seconds(2));
  ASSERT_FALSE(out);
  EXPECT_TRUE(out.error().kind == WhepErrorKind::kNetwork || out.error().kind == WhepErrorKind::kTimeout);
}

TEST(WhepPost, CrossOriginAbsoluteLocationRejected) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  ix::WebSocketHttpHeaders h;
  h["Location"] = "http://evil.example:9/steal";
  stub.setReply(201, h, "ANSWER");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "", "OFFER", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kBadResponse);
}

TEST(WhepPost, SameOriginAbsoluteLocationAccepted) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  ix::WebSocketHttpHeaders h;
  h["Location"] = stub.baseUrl() + "/cam0/whep/s1";
  stub.setReply(201, h, "ANSWER");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "", "OFFER", std::chrono::seconds(5));
  ASSERT_TRUE(out);
  EXPECT_EQ(out->session_url, stub.baseUrl() + "/cam0/whep/s1");
}

TEST(WhepPost, WrongAnswerContentTypeIsBadResponse) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  ix::WebSocketHttpHeaders h;
  h["Location"] = "/cam0/whep/s1";
  h["Content-Type"] = "text/html";
  stub.setReply(201, h, "<html>login page</html>");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "", "OFFER", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kBadResponse);
}

TEST(WhepPost, ControlCharTokenRejectedBeforeSending) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  ix::WebSocketHttpHeaders h;
  h["Location"] = "/s/1";
  stub.setReply(201, h, "ANSWER");
  auto out = postOffer(stub.baseUrl() + "/cam0/whep", "tok\r\nX-Evil: 1", "OFFER", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kUnauthorized);
  EXPECT_TRUE(stub.seen().empty());  // rejected before any request went out
}

TEST(WhepDelete, SendsDeleteWithBearer) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(200, {}, "");
  deleteSession(stub.baseUrl() + "/cam0/whep/session-1", "tok123", std::chrono::seconds(5));
  auto seen = stub.seen();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].method, "DELETE");
  EXPECT_EQ(seen[0].uri, "/cam0/whep/session-1");
  EXPECT_EQ(seen[0].authorization, "Bearer tok123");
}

TEST(WhepPathsList, ParsesNameReadyTracks) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(
      200, {},
      R"({"itemCount":2,"pageCount":1,"items":[)"
      R"({"name":"cam0","ready":true,"tracks":["H264"]},)"
      R"({"name":"cams/front","ready":false,"tracks":["H264","MPEG-4 Audio"]}]})");
  auto out = fetchPathsList(stub.baseUrl(), "tokX", std::chrono::seconds(5));
  ASSERT_TRUE(out);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].name, "cam0");
  EXPECT_TRUE((*out)[0].ready);
  ASSERT_EQ((*out)[0].tracks.size(), 1u);
  EXPECT_EQ((*out)[0].tracks[0], "H264");
  EXPECT_EQ((*out)[1].name, "cams/front");
  EXPECT_FALSE((*out)[1].ready);
  EXPECT_EQ((*out)[1].tracks.size(), 2u);

  auto seen = stub.seen();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].method, "GET");
  EXPECT_EQ(seen[0].uri, "/v3/paths/list?page=0");
  EXPECT_EQ(seen[0].authorization, "Bearer tokX");
}

TEST(WhepPathsList, MalformedJsonIsBadResponse) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(200, {}, "not json");
  auto out = fetchPathsList(stub.baseUrl(), "", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kBadResponse);
}

TEST(WhepPathsList, Unauthorized401IsTyped) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(401, {}, "");
  auto out = fetchPathsList(stub.baseUrl(), "", std::chrono::seconds(5));
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, WhepErrorKind::kUnauthorized);
}

TEST(WhepPathsList, NonObjectItemsAreSkippedNotFatal) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(200, {}, R"({"items":[42,{"name":"cam0","ready":true,"tracks":["H264"]},null,"junk"]})");
  auto out = fetchPathsList(stub.baseUrl(), "", std::chrono::seconds(5));
  ASSERT_TRUE(out);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_EQ((*out)[0].name, "cam0");
}

TEST(WhepPathsList, WrongTypedFieldsSkipTheItemNotFatal) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReply(
      200, {},
      R"({"items":[{"name":"bad","ready":"yes","tracks":"H264"},)"
      R"({"name":"cam0","ready":true,"tracks":["H264"]}]})");
  auto out = fetchPathsList(stub.baseUrl(), "", std::chrono::seconds(5));
  ASSERT_TRUE(out);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_EQ((*out)[0].name, "cam0");
}

TEST(WhepPathsList, FollowsPagination) {
  StubWhepServer stub;
  ASSERT_TRUE(stub.listening());
  stub.setReplies({
      {200, {}, R"({"pageCount":2,"items":[{"name":"cam0","ready":true,"tracks":["H264"]}]})"},
      {200, {}, R"({"pageCount":2,"items":[{"name":"cam1","ready":true,"tracks":["H264"]}]})"},
  });
  auto out = fetchPathsList(stub.baseUrl(), "", std::chrono::seconds(5));
  ASSERT_TRUE(out);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].name, "cam0");
  EXPECT_EQ((*out)[1].name, "cam1");

  auto seen = stub.seen();
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0].method, "GET");
  EXPECT_EQ(seen[0].uri, "/v3/paths/list?page=0");
  EXPECT_EQ(seen[1].uri, "/v3/paths/list?page=1");
}

}  // namespace
}  // namespace webrtc
}  // namespace PJ
