/**
 * @file url_test.cpp
 * @brief normalizeUrl (what the dialog's URL line dials) and cleanUrl (the remembered copy).
 */

#include <gtest/gtest.h>

#include <pj_websocket/url.hpp>
#include <string>

namespace {

using PJ::WebSocket::cleanUrl;
using PJ::WebSocket::normalizeUrl;
using PJ::WebSocket::withCleanUrl;

std::string norm(std::string_view text) {
  auto url = normalizeUrl(text, 9090);
  EXPECT_TRUE(url.has_value()) << text << ": " << (url ? "" : url.error());
  return url ? *url : std::string{};
}

std::string normError(std::string_view text) {
  auto url = normalizeUrl(text, 9090);
  EXPECT_FALSE(url.has_value()) << text << " -> " << (url ? *url : "");
  return url ? std::string{} : url.error();
}

TEST(NormalizeUrl, FullUrlsAreKeptAsTyped) {
  EXPECT_EQ(norm("ws://127.0.0.1:9090"), "ws://127.0.0.1:9090");
  EXPECT_EQ(norm("wss://robot.local:8443/bridge?x=1"), "wss://robot.local:8443/bridge?x=1");
  EXPECT_EQ(norm("ws://[::1]:9090"), "ws://[::1]:9090");
  // A scheme without a port keeps the scheme's own port: no default is invented.
  EXPECT_EQ(norm("wss://robot.example.com/bridge"), "wss://robot.example.com/bridge");
}

TEST(NormalizeUrl, ForgivingInput) {
  EXPECT_EQ(norm("  robot  "), "ws://robot:9090");
  EXPECT_EQ(norm("robot:1234"), "ws://robot:1234");
  EXPECT_EQ(norm("10.0.0.2/bridge?x=1"), "ws://10.0.0.2:9090/bridge?x=1");
  // "xxx://" is a scheme only when xxx has none of /?#@: here it is part of the query.
  EXPECT_EQ(norm("robot/x?next=ws://a"), "ws://robot:9090/x?next=ws://a");
  EXPECT_EQ(norm("robot?u=wss://b:1"), "ws://robot:9090/?u=wss://b:1");
  EXPECT_EQ(norm("[::1]"), "ws://[::1]:9090");
  EXPECT_EQ(norm("WSS://Robot:1"), "wss://Robot:1");  // only the scheme is lowercased
}

TEST(NormalizeUrl, AQueryRightAfterThePortGetsAPath) {
  // IXWebSocket reads the port up to the next '/': without one it would swallow the query.
  EXPECT_EQ(norm("robot:9090?token=abc"), "ws://robot:9090/?token=abc");
  EXPECT_EQ(norm("ws://robot:9090?token=abc"), "ws://robot:9090/?token=abc");
  EXPECT_EQ(norm("wss://robot?token=abc"), "wss://robot/?token=abc");
}

TEST(NormalizeUrl, InvalidInputSaysWhy) {
  EXPECT_NE(normError("").find("Enter the server URL"), std::string::npos);
  EXPECT_NE(normError("   ").find("ws://127.0.0.1:9090"), std::string::npos);
  EXPECT_NE(normError("http://robot:80").find("ws:// or wss://"), std::string::npos);
  EXPECT_NE(normError("ws://ro bot").find("spaces"), std::string::npos);
  EXPECT_NE(normError("ws://").find("no host"), std::string::npos);
  EXPECT_NE(normError("ws:///path").find("no host"), std::string::npos);
  EXPECT_NE(normError(":9090").find("no host"), std::string::npos);
  EXPECT_NE(normError("robot:abc").find("Invalid port"), std::string::npos);
  EXPECT_NE(normError("robot:70000").find("Invalid port"), std::string::npos);
  EXPECT_NE(normError("robot:").find("Invalid port"), std::string::npos);
  EXPECT_NE(normError("ws://robot:0").find("Invalid port"), std::string::npos);
  EXPECT_NE(normError("::1").find("brackets"), std::string::npos);
  EXPECT_NE(normError("ws://[::1").find("']'"), std::string::npos);
}

TEST(NormalizeUrl, RefusesCredentials) {
  // IXWebSocket parses and drops userinfo and browsers do not send it: refused, never silently lost.
  const std::string kWhy = "Credentials in the URL are not supported: neither PlotJuggler nor browsers send them.";
  EXPECT_EQ(normError("ws://user:pw@robot:9090/a"), kWhy);
  EXPECT_EQ(normError("wss://user@robot"), kWhy);
  EXPECT_EQ(normError("user:pw@robot"), kWhy);
  EXPECT_EQ(normError("u@robot?next=ws://a"), kWhy);
  // An '@' after the authority is not userinfo.
  EXPECT_EQ(norm("ws://robot:1/x@y?m=a@b"), "ws://robot:1/x@y?m=a@b");
}

TEST(NormalizeUrl, RefusesAFragment) {
  // RFC 6455 forbids fragments in WebSocket URIs.
  EXPECT_NE(normError("wss://robot.local:8443/bridge?x=1#f").find("'#'"), std::string::npos);
  EXPECT_NE(normError("robot#top").find("'#'"), std::string::npos);
  EXPECT_EQ(norm("ws://robot/a%23b"), "ws://robot/a%23b");  // an encoded '#' is not a fragment
}

// normalizeUrl refuses userinfo, but a status line or a log may still show text it refused.
TEST(CleanUrl, DropsUserinfo) {
  EXPECT_EQ(cleanUrl("ws://user:secret@robot:9090/a"), "ws://robot:9090/a");
  EXPECT_EQ(cleanUrl("user:secret@robot:9090"), "robot:9090");
  EXPECT_EQ(cleanUrl("wss://user@robot"), "wss://robot");
  // The last '@' of the authority ends the userinfo; an '@' in the path is not userinfo.
  EXPECT_EQ(cleanUrl("ws://u:p@ss@robot:1/x@y"), "ws://robot:1/x@y");
}

TEST(CleanUrl, DropsTheWholeQueryAndFragment) {
  EXPECT_EQ(cleanUrl("ws://h:1/p?token=t&ns=a"), "ws://h:1/p");
  EXPECT_EQ(cleanUrl("ws://h:1/?keyframe=1"), "ws://h:1/");
  EXPECT_EQ(cleanUrl("ws://h:1/p?"), "ws://h:1/p");
  EXPECT_EQ(cleanUrl("ws://h:1/p#access_token=t"), "ws://h:1/p");
  EXPECT_EQ(cleanUrl("robot:1?next=ws://u:p@a"), "robot:1");  // an embedded URL in the query is not a scheme
}

TEST(CleanUrl, EverythingElseIsByteIdentical) {
  EXPECT_EQ(cleanUrl("wss://robot.local:8443/bridge"), "wss://robot.local:8443/bridge");
  EXPECT_EQ(cleanUrl("ws://[::1]:9090/a%20b"), "ws://[::1]:9090/a%20b");
  // A secret in the path cannot be detected.
  EXPECT_EQ(cleanUrl("ws://h:1/s3cr3t-path"), "ws://h:1/s3cr3t-path");
}

TEST(WithCleanUrl, CleansEveryQuoteOfTheUrl) {
  EXPECT_EQ(withCleanUrl("to ws://h/p?t=1, again ws://h/p?t=1", "ws://h/p?t=1"), "to ws://h/p, again ws://h/p");
  EXPECT_EQ(withCleanUrl("refused", "ws://h/p?t=1"), "refused");
  EXPECT_EQ(withCleanUrl("x", ""), "x");
}

}  // namespace
