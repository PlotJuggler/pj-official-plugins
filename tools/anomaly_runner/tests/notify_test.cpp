// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the PURE notification logic: ${ENV} expansion, config parse +
// validation, the fire predicate, and payload building. Network delivery
// (webhook/email) is exercised separately (command-sink end-to-end + manual);
// these tests need no network.

#include "notify.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

using namespace anomaly_notify;

namespace {

nlohmann::json reportDoc(const std::string& status, int info = 0, int warning = 0, int error = 0, int critical = 0) {
  return nlohmann::json{
      {"file", "run.csv"},
      {"status", status},
      {"summary",
       {{"total", info + warning + error + critical},
        {"by_severity", {{"info", info}, {"warning", warning}, {"error", error}, {"critical", critical}}}}}};
}

}  // namespace

// --------------------------------------------------------------------------- env

TEST(ExpandEnv, SubstitutesAndEscapes) {
  ::setenv("ANOMALY_TEST_TOKEN", "s3cr3t", 1);
  EXPECT_EQ(expandEnv("Bearer ${ANOMALY_TEST_TOKEN}"), "Bearer s3cr3t");
  EXPECT_EQ(expandEnv("${ANOMALY_TEST_TOKEN}/${ANOMALY_TEST_TOKEN}"), "s3cr3t/s3cr3t");
  EXPECT_EQ(expandEnv("cost is $$5"), "cost is $5");  // "$$" -> literal "$"
  EXPECT_EQ(expandEnv("plain text"), "plain text");
}

TEST(ExpandEnv, UnsetVariableBecomesEmpty) {
  ::unsetenv("ANOMALY_TEST_UNSET");
  EXPECT_EQ(expandEnv("x${ANOMALY_TEST_UNSET}y"), "xy");
}

// ------------------------------------------------------------------------ config

TEST(ParseConfig, WebhookWithEnvHeader) {
  ::setenv("ANOMALY_TEST_TOKEN", "abc123", 1);
  std::string err;
  auto cfg = parseConfig(
      R"({
    "notify_on": "fail",
    "sinks": [{ "type": "webhook", "url": "https://example.com/hook",
                "headers": { "Authorization": "Bearer ${ANOMALY_TEST_TOKEN}" } }]
  })",
      &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->notify_on, "fail");
  ASSERT_EQ(cfg->sinks.size(), 1u);
  EXPECT_EQ(cfg->sinks[0].transport, Transport::kWebhook);
  EXPECT_EQ(cfg->sinks[0].url, "https://example.com/hook");
  ASSERT_EQ(cfg->sinks[0].headers.size(), 1u);
  EXPECT_EQ(cfg->sinks[0].headers[0].first, "Authorization");
  EXPECT_EQ(cfg->sinks[0].headers[0].second, "Bearer abc123");
}

TEST(ParseConfig, EmailAndCommandSinks) {
  std::string err;
  auto cfg = parseConfig(
      R"({
    "notify_on": "always",
    "sinks": [
      { "type": "email", "smtp_url": "smtp://mail:25", "from": "a@x", "to": ["b@y", "c@z"] },
      { "type": "command", "exec": ["sh", "-c", "cat"] }
    ]
  })",
      &err);
  ASSERT_TRUE(cfg) << err;
  ASSERT_EQ(cfg->sinks.size(), 2u);
  EXPECT_EQ(cfg->sinks[0].transport, Transport::kEmail);
  EXPECT_EQ(cfg->sinks[0].from, "a@x");
  EXPECT_EQ(cfg->sinks[0].to.size(), 2u);
  EXPECT_EQ(cfg->sinks[1].transport, Transport::kCommand);
  EXPECT_EQ(cfg->sinks[1].exec.size(), 3u);
  EXPECT_EQ(cfg->sinks[1].exec[0], "sh");
}

TEST(ParseConfig, RejectsMalformed) {
  std::string err;
  EXPECT_FALSE(parseConfig("{ not json", &err));
  EXPECT_FALSE(parseConfig(R"({"notify_on":"fail"})", &err));             // no sinks
  EXPECT_FALSE(parseConfig(R"({"notify_on":"fail","sinks":[]})", &err));  // empty sinks
  EXPECT_FALSE(parseConfig(R"({"notify_on":"sometimes","sinks":[{"type":"webhook","url":"u"}]})", &err));
  EXPECT_FALSE(parseConfig(R"({"sinks":[{"type":"webhook"}]})", &err));                      // webhook missing url
  EXPECT_FALSE(parseConfig(R"({"sinks":[{"type":"email","from":"a","to":["b"]}]})", &err));  // email missing smtp_url
  EXPECT_FALSE(parseConfig(R"({"sinks":[{"type":"command","exec":[]}]})", &err));            // empty exec
  EXPECT_FALSE(parseConfig(R"({"sinks":[{"type":"smoke"}]})", &err));                        // unknown type
}

TEST(ParseConfig, SeverityPolicyValidation) {
  std::string err;
  EXPECT_TRUE(parseConfig(R"({"notify_on":"severity>=warning","sinks":[{"type":"command","exec":["x"]}]})", &err))
      << err;
  EXPECT_FALSE(parseConfig(R"({"notify_on":"severity>=loud","sinks":[{"type":"command","exec":["x"]}]})", &err));
}

TEST(ParseConfig, DefaultsNotifyOnToFail) {
  std::string err;
  auto cfg = parseConfig(R"({"sinks":[{"type":"command","exec":["x"]}]})", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->notify_on, "fail");
}

// ----------------------------------------------------------------------- predicate

TEST(ShouldNotify, AlwaysAndFail) {
  EXPECT_TRUE(shouldNotify(reportDoc("pass"), "always"));
  EXPECT_TRUE(shouldNotify(reportDoc("fail"), "always"));
  EXPECT_TRUE(shouldNotify(reportDoc("fail"), "fail"));
  EXPECT_FALSE(shouldNotify(reportDoc("pass"), "fail"));
}

TEST(ShouldNotify, SeverityThreshold) {
  // by_severity: only a warning present.
  const auto warn = reportDoc("pass", /*info=*/0, /*warning=*/2);
  EXPECT_TRUE(shouldNotify(warn, "severity>=info"));
  EXPECT_TRUE(shouldNotify(warn, "severity>=warning"));
  EXPECT_FALSE(shouldNotify(warn, "severity>=error"));
  EXPECT_FALSE(shouldNotify(warn, "severity>=critical"));

  const auto crit = reportDoc("fail", 0, 0, 0, /*critical=*/1);
  EXPECT_TRUE(shouldNotify(crit, "severity>=error"));
  EXPECT_TRUE(shouldNotify(crit, "severity>=critical"));
}

// ------------------------------------------------------------------------- payload

TEST(Payload, DefaultSubject) {
  EXPECT_EQ(defaultSubject(reportDoc("fail", 0, 0, 7)), "[anomaly] FAIL: 7 marker(s) — run.csv");
}

TEST(Payload, EmailMessageHasHeadersAndBody) {
  Sink s;
  s.transport = Transport::kEmail;
  s.from = "robot@lab";
  s.to = {"eng@lab", "qa@lab"};
  const std::string msg = buildEmailMessage(s, "Subj", R"({"k":1})");
  EXPECT_NE(msg.find("From: robot@lab\r\n"), std::string::npos);
  EXPECT_NE(msg.find("To: eng@lab, qa@lab\r\n"), std::string::npos);
  EXPECT_NE(msg.find("Subject: Subj\r\n"), std::string::npos);
  EXPECT_NE(msg.find("\r\n\r\n"), std::string::npos);  // header/body separator
  EXPECT_NE(msg.find(R"({"k":1})"), std::string::npos);
}

// ------------------------------------------------------------- dispatch (command sink)

TEST(Dispatch, PolicyNotFiredAttemptsNothing) {
  std::string err;
  auto cfg = parseConfig(R"({"notify_on":"fail","sinks":[{"type":"command","exec":["false"]}]})", &err);
  ASSERT_TRUE(cfg) << err;
  const auto r = dispatch("{}", reportDoc("pass"), *cfg);  // pass + notify_on:fail -> no fire
  EXPECT_EQ(r.fired, 0);
  EXPECT_TRUE(r.allOk());
}

TEST(Dispatch, CommandSinkReceivesReportOnStdin) {
  const std::string out_path = "/tmp/anomaly_notify_cmd_test.json";
  ::unlink(out_path.c_str());
  std::string err;
  auto cfg = parseConfig(
      R"({"notify_on":"always","sinks":[{"type":"command","exec":["sh","-c","cat > )" + out_path + R"("]}]})", &err);
  ASSERT_TRUE(cfg) << err;
  const std::string payload = R"({"status":"fail","n":42})";
  const auto r = dispatch(payload, reportDoc("fail"), *cfg);
  EXPECT_EQ(r.fired, 1);
  EXPECT_EQ(r.succeeded, 1);
  EXPECT_TRUE(r.allOk());

  std::FILE* f = std::fopen(out_path.c_str(), "rb");
  ASSERT_NE(f, nullptr);
  std::string got;
  char buf[256];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    got.append(buf, n);
  }
  std::fclose(f);
  EXPECT_EQ(got, payload);
}

TEST(Dispatch, CommandSinkFailurePropagates) {
  std::string err;
  auto cfg = parseConfig(R"({"notify_on":"always","sinks":[{"type":"command","exec":["false"]}]})", &err);
  ASSERT_TRUE(cfg) << err;
  const auto r = dispatch("{}", reportDoc("fail"), *cfg);
  EXPECT_EQ(r.fired, 1);
  EXPECT_EQ(r.succeeded, 0);
  EXPECT_FALSE(r.allOk());
  ASSERT_EQ(r.errors.size(), 1u);
}
