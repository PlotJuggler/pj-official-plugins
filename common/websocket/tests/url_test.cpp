/**
 * @file url_test.cpp
 * @brief normalizeUrl (what the dialog's URL line dials) and cleanUrl (the remembered copy).
 */

#include <gtest/gtest.h>

#include <pj_websocket/url.hpp>
#include <string>

namespace {

using PJ::WebSocket::cleanUrl;
using PJ::WebSocket::isSecretQueryName;
using PJ::WebSocket::normalizeUrl;

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
  EXPECT_EQ(norm("wss://robot.local:8443/bridge?x=1#f"), "wss://robot.local:8443/bridge?x=1#f");
  EXPECT_EQ(norm("ws://user:p%40ss@robot:9090/a"), "ws://user:p%40ss@robot:9090/a");
  EXPECT_EQ(norm("ws://[::1]:9090"), "ws://[::1]:9090");
  // A scheme without a port keeps the scheme's own port: no default is invented.
  EXPECT_EQ(norm("wss://robot.example.com/bridge"), "wss://robot.example.com/bridge");
}

TEST(NormalizeUrl, ForgivingInput) {
  EXPECT_EQ(norm("  robot  "), "ws://robot:9090");
  EXPECT_EQ(norm("robot:1234"), "ws://robot:1234");
  EXPECT_EQ(norm("10.0.0.2/bridge?x=1"), "ws://10.0.0.2:9090/bridge?x=1");
  EXPECT_EQ(norm("user:pw@robot"), "ws://user:pw@robot:9090");
  EXPECT_EQ(norm("[::1]"), "ws://[::1]:9090");
  EXPECT_EQ(norm("WSS://Robot:1"), "wss://Robot:1");  // only the scheme is lowercased
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

TEST(CleanUrl, DropsUserinfo) {
  EXPECT_EQ(cleanUrl("ws://user:secret@robot:9090/a?x=1"), "ws://robot:9090/a?x=1");
  EXPECT_EQ(cleanUrl("wss://user@robot"), "wss://robot");
  // The last '@' of the authority ends the userinfo; an '@' in the path or query is not userinfo.
  EXPECT_EQ(cleanUrl("ws://u:p@ss@robot:1/x@y?m=a@b"), "ws://robot:1/x@y?m=a@b");
}

TEST(CleanUrl, DropsEachKindOfSecretParameter) {
  for (const char* name :
       {"password", "passwd", "secret", "token", "apikey", "privatekey", "credential", "access_token", "X-Api-Key",
        "client_secret", "API_KEY", "credentials", "id%5Ftoken"}) {
    EXPECT_EQ(cleanUrl(std::string("ws://h:1/p?a=1&") + name + "=v&b=2"), "ws://h:1/p?a=1&b=2") << name;
  }
  for (const char* name :
       {"key", "auth", "authorization", "sig", "signature", "pwd", "pass", "session", "jwt", "KEY", "Auth"}) {
    EXPECT_EQ(cleanUrl(std::string("ws://h:1/p?a=1&") + name + "=v&b=2"), "ws://h:1/p?a=1&b=2") << name;
  }
}

TEST(CleanUrl, KeepsLookAlikesAndOrder) {
  // Short names match exactly only.
  for (const char* name : {"keyframe", "passive", "session_id", "author", "signal", "monkey", "jwtx"}) {
    EXPECT_FALSE(isSecretQueryName(name)) << name;
  }
  EXPECT_EQ(cleanUrl("ws://h:1/p?z=1&keyframe=2&token=t&a=3&passive=4"), "ws://h:1/p?z=1&keyframe=2&a=3&passive=4");
}

TEST(CleanUrl, EverythingElseIsByteIdentical) {
  EXPECT_EQ(cleanUrl("wss://robot.local:8443/bridge"), "wss://robot.local:8443/bridge");
  EXPECT_EQ(cleanUrl("ws://h:1/p?a=1&&b=2&c"), "ws://h:1/p?a=1&&b=2&c");
  EXPECT_EQ(cleanUrl("ws://h:1/p?"), "ws://h:1/p?");
  EXPECT_EQ(cleanUrl("ws://h:1/p?token=x"), "ws://h:1/p");  // a query left empty loses its '?'
  EXPECT_EQ(cleanUrl("ws://h:1/p?token=x#frag"), "ws://h:1/p#frag");
  EXPECT_EQ(cleanUrl("ws://h:1/p?a=1#token=x"), "ws://h:1/p?a=1#token=x");  // the fragment is not the query
  EXPECT_EQ(cleanUrl("ws://[::1]:9090/?k=v"), "ws://[::1]:9090/?k=v");
  // A secret in the path cannot be detected.
  EXPECT_EQ(cleanUrl("ws://h:1/s3cr3t-path"), "ws://h:1/s3cr3t-path");
}

}  // namespace
