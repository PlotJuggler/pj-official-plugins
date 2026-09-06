// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Mosaico source descriptor: parse/validate round trip, canonical
// serialization + identity conformance against the vectors file
// (MOSAICO_VECTORS_JSON), display_name identity invariance, and the strict
// rejection matrix (allowlist at both levels, limits, origin hygiene,
// ns-string syntax, range order, non-empty explicit topic list).
#include "source_descriptor.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace {

using mosaico::SourceDescriptor;
namespace descriptor_import = PJ::sdk::descriptor_import;

SourceDescriptor fullDescriptor() {
  SourceDescriptor d;
  d.version = 1;
  d.kind = "mosaico.pull";
  d.origin = "demo.mosaico.dev:6726";
  d.sequence = "bonirob_2016-04-20-15-42-15_7";
  d.topics = {"/camera/image_raw", "/gps", "/imu"};
  d.start_ns = 1461159735713519717LL;
  d.end_ns = 1461159795713519718LL;
  d.display_name = "Bonirob run 7";
  return d;
}

// A minimal valid descriptor as mutable JSON — the rejection matrix mutates
// exactly one aspect per case so each error is attributable.
nlohmann::json baseJson() {
  return nlohmann::json{
      {"v", 1},
      {"kind", "mosaico.pull"},
      {"request",
       nlohmann::json{
           {"origin", "demo.mosaico.dev:6726"},
           {"sequence", "seq_a"},
           {"topics", nlohmann::json::array({"/imu"})},
           {"start_ns", "0"},
           {"end_ns", "0"},
       }},
      {"display_name", "A"},
  };
}

void expectReject(const std::string& json, const std::string& error_substr) {
  std::string error;
  const auto d = mosaico::parseSourceDescriptor(json, &error);
  EXPECT_FALSE(d.has_value()) << "accepted: " << json.substr(0, 200);
  EXPECT_NE(error.find(error_substr), std::string::npos)
      << "error \"" << error << "\" lacks substring \"" << error_substr << "\"";
}

std::string slurp(const char* path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << "cannot open vectors file " << path;
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

}  // namespace

TEST(SourceDescriptor, RoundTrip) {
  const SourceDescriptor d = fullDescriptor();
  std::string error;
  const auto parsed = mosaico::parseSourceDescriptor(mosaico::toSourceDescriptorJson(d), &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->version, d.version);
  EXPECT_EQ(parsed->kind, d.kind);
  EXPECT_EQ(parsed->origin, d.origin);
  EXPECT_EQ(parsed->sequence, d.sequence);
  EXPECT_EQ(parsed->topics, d.topics);
  EXPECT_EQ(parsed->start_ns, d.start_ns);
  EXPECT_EQ(parsed->end_ns, d.end_ns);
  EXPECT_EQ(parsed->display_name, d.display_name);
}

// A canonical string must itself parse (dedup/cache comparison feeds it back
// in), even though it lacks display_name.
TEST(SourceDescriptor, CanonicalFormParses) {
  const SourceDescriptor d = fullDescriptor();
  std::string error;
  const auto parsed = mosaico::parseSourceDescriptor(mosaico::canonicalSourceDescriptorJson(d), &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_TRUE(parsed->display_name.empty());
  EXPECT_EQ(mosaico::descriptorIdentity(*parsed), mosaico::descriptorIdentity(d));
}

// The vectors are the canonicalization contract: every case's descriptor must
// parse, and its canonical bytes + identity must match the file verbatim. The
// "display-name-does-not-change-identity" case is doubly asserted: a twin
// differing ONLY in display_name yields the same canonical bytes and the same
// identity (display_name is excluded from the digest).
TEST(SourceDescriptor, VectorConformance) {
  const std::string raw = slurp(MOSAICO_VECTORS_JSON);
  const auto vectors = nlohmann::json::parse(raw, /*cb=*/nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(vectors.is_discarded());
  ASSERT_TRUE(vectors.contains("cases") && vectors["cases"].is_array());
  ASSERT_GE(vectors["cases"].size(), 3u);

  for (const auto& c : vectors["cases"]) {
    const std::string name = c["name"].get<std::string>();
    const std::string descriptor_json = c["descriptor"].dump();
    SCOPED_TRACE(name);

    const auto sdk_descriptor =
        descriptor_import::parseSourceDescriptor(descriptor_json, mosaico::sourceDescriptorPolicy());
    ASSERT_TRUE(sdk_descriptor) << sdk_descriptor.error();
    EXPECT_EQ(
        descriptor_import::canonicalSourceDescriptorJson(*sdk_descriptor, mosaico::sourceDescriptorPolicy()),
        c["canonical"].get<std::string>());
    EXPECT_EQ(
        descriptor_import::sourceDescriptorIdentity(*sdk_descriptor, mosaico::sourceDescriptorPolicy()),
        c["identity"].get<std::string>());

    std::string error;
    const auto d = mosaico::parseSourceDescriptor(descriptor_json, &error);
    ASSERT_TRUE(d.has_value()) << error;
    EXPECT_EQ(mosaico::canonicalSourceDescriptorJson(*d), c["canonical"].get<std::string>());
    EXPECT_EQ(mosaico::descriptorIdentity(*d), c["identity"].get<std::string>());

    if (name == "display-name-does-not-change-identity") {
      SourceDescriptor twin = *d;
      twin.display_name = "X";
      EXPECT_EQ(mosaico::canonicalSourceDescriptorJson(twin), mosaico::canonicalSourceDescriptorJson(*d));
      EXPECT_EQ(mosaico::descriptorIdentity(twin), mosaico::descriptorIdentity(*d));
    }
  }
}

TEST(SourceDescriptor, IdentityInvariance) {
  const SourceDescriptor d = fullDescriptor();
  SourceDescriptor renamed = d;
  renamed.display_name = "RENAMED";
  EXPECT_EQ(mosaico::descriptorIdentity(renamed), mosaico::descriptorIdentity(d));

  SourceDescriptor narrowed = d;
  narrowed.end_ns -= 1;
  EXPECT_NE(mosaico::descriptorIdentity(narrowed), mosaico::descriptorIdentity(d));

  SourceDescriptor other_topics = d;
  other_topics.topics = {"/imu"};
  EXPECT_NE(mosaico::descriptorIdentity(other_topics), mosaico::descriptorIdentity(d));

  SourceDescriptor other_origin = d;
  other_origin.origin = "other.mosaico.dev:6726";
  EXPECT_NE(mosaico::descriptorIdentity(other_origin), mosaico::descriptorIdentity(d));
}

// The producer path must emit EXACTLY what the parser re-accepts: the source
// record the fetch worker attaches is toSourceDescriptorJson of a
// makePullDescriptor result, and (with no display_name) those bytes are the
// canonical bytes the identity is computed over.
TEST(SourceDescriptor, ProducerBytesAreCanonical) {
  std::string error;
  const auto made = mosaico::makePullDescriptor("demo.mosaico.dev:6726", "seq", {"/b", "/a"}, 1, 2, &error);
  ASSERT_TRUE(made.has_value()) << error;
  EXPECT_EQ(mosaico::toSourceDescriptorJson(*made), mosaico::canonicalSourceDescriptorJson(*made));
}

TEST(SourceDescriptor, RejectionMatrix) {
  {  // Unsupported version.
    auto j = baseJson();
    j["v"] = 2;
    expectReject(j.dump(), "version");
  }
  {  // Version narrowing: 2^32+1 must NOT alias v1 through an unchecked
     // get<int>() cast — exact-value rejection, fail-closed.
    auto j = baseJson();
    j["v"] = 4294967297ull;
    expectReject(j.dump(), "version");
  }
  {  // Negative wraparound sibling of the same bug class.
    auto j = baseJson();
    j["v"] = -4294967295ll;
    expectReject(j.dump(), "version");
  }
  {  // Missing required field.
    auto j = baseJson();
    j.erase("kind");
    expectReject(j.dump(), "kind");
  }
  {  // Wrong kind.
    auto j = baseJson();
    j["kind"] = "mosaico-sequence";
    expectReject(j.dump(), "kind");
  }
  {  // Unknown top-level field (strict allowlist — a token must never ride
     // along).
    auto j = baseJson();
    j["api_key"] = "x";
    expectReject(j.dump(), "unknown field");
  }
  {  // Unknown field NESTED in request — the SDK guards only the top level,
     // so the typed parser must close this hole itself.
    auto j = baseJson();
    j["request"]["api_key"] = "x";
    expectReject(j.dump(), "unknown field");
  }
  {  // allow_insecure is a per-machine choice, never part of the request.
    auto j = baseJson();
    j["request"]["allow_insecure"] = true;
    expectReject(j.dump(), "unknown field");
  }
  {  // Origin userinfo (credential smuggling) — rejected by charset.
    auto j = baseJson();
    j["request"]["origin"] = "user:pw@h:6726";
    expectReject(j.dump(), "origin");
  }
  {  // Origin with scheme.
    auto j = baseJson();
    j["request"]["origin"] = "grpc+tls://h:6726";
    expectReject(j.dump(), "origin");
  }
  {  // Origin with path/query/fragment.
    auto j = baseJson();
    j["request"]["origin"] = "h:6726/path?x=1#f";
    expectReject(j.dump(), "origin");
  }
  {  // Uppercase host: only the canonical lowercase spelling has an identity.
    auto j = baseJson();
    j["request"]["origin"] = "Demo.Mosaico.DEV:6726";
    expectReject(j.dump(), "origin");
  }
  {  // Missing port.
    auto j = baseJson();
    j["request"]["origin"] = "demo.mosaico.dev";
    expectReject(j.dump(), "origin");
  }
  {  // IPv6 literal (extra colons fail the port syntax).
    auto j = baseJson();
    j["request"]["origin"] = "[::1]:6726";
    expectReject(j.dump(), "origin");
  }
  {  // Leading-zero port would alias another spelling of the same origin.
    auto j = baseJson();
    j["request"]["origin"] = "h:06726";
    expectReject(j.dump(), "port");
  }
  {  // Empty sequence name.
    auto j = baseJson();
    j["request"]["sequence"] = "";
    expectReject(j.dump(), "sequence");
  }
  {  // Topic-count limit.
    auto j = baseJson();
    auto topics = nlohmann::json::array();
    for (std::size_t i = 0; i < mosaico::kMaxTopics + 1; ++i) {
      topics.push_back("/t" + std::to_string(i));
    }
    j["request"]["topics"] = topics;
    expectReject(j.dump(), "entry limit");
  }
  {  // Non-numeric ns string.
    auto j = baseJson();
    j["request"]["start_ns"] = "abc";
    expectReject(j.dump(), "decimal");
  }
  {  // Signed ns string (the wire format is unsigned decimal digits only).
    auto j = baseJson();
    j["request"]["start_ns"] = "-5";
    expectReject(j.dump(), "decimal");
  }
  {  // Numeric (non-string) ns: int64 must ride as a decimal string.
    auto j = baseJson();
    j["request"]["start_ns"] = 5;
    expectReject(j.dump(), "string");
  }
  {  // Inverted range (allowed only as the "0"/"0" unbounded sentinel).
    auto j = baseJson();
    j["request"]["start_ns"] = "9";
    j["request"]["end_ns"] = "5";
    expectReject(j.dump(), "before start_ns");
  }
  {  // Whole-input size limit (checked before parsing).
    const std::string oversized(mosaico::kMaxDescriptorBytes + 1, ' ');
    expectReject(oversized, "byte limit");
  }
}

// The exact-replay contract: the descriptor always carries the explicit topic
// list that was fetched (selection expands before capture), so an empty list
// is a contract error, and empty/duplicate names would only fail deep in the
// server on replay.
TEST(SourceDescriptor, RejectsEmptyTopicListEmptyNamesAndDuplicates) {
  {  // Empty topics array — no "empty = all" shorthand exists here.
    auto j = baseJson();
    j["request"]["topics"] = nlohmann::json::array();
    expectReject(j.dump(), "at least one");
  }
  {  // Empty-string topic.
    auto j = baseJson();
    j["request"]["topics"] = nlohmann::json::array({""});
    expectReject(j.dump(), "empty");
  }
  {  // Duplicate topics.
    auto j = baseJson();
    j["request"]["topics"] = nlohmann::json::array({"/a", "/a", "/b"});
    expectReject(j.dump(), "duplicate");
  }
  {  // Unsorted topics: production sorts, so canonical bytes are unique only
     // if the parser refuses any other order instead of re-sorting it.
    auto j = baseJson();
    j["request"]["topics"] = nlohmann::json::array({"/b", "/a"});
    expectReject(j.dump(), "sorted");
  }
}

TEST(SourceDescriptorFactory, StampsKindAndSortsTopics) {
  // The identity is a property of the topic SET: the factory sorts + dedups,
  // so two selections of the same topics in different orders share one
  // identity.
  std::string error;
  const auto ab = mosaico::makePullDescriptor("demo.mosaico.dev:6726", "seq", {"/b", "/a", "/b"}, 1, 2, &error);
  ASSERT_TRUE(ab.has_value()) << error;
  EXPECT_EQ(ab->kind, "mosaico.pull");
  EXPECT_EQ(ab->topics, (std::vector<std::string>{"/a", "/b"}));
  const auto ba = mosaico::makePullDescriptor("demo.mosaico.dev:6726", "seq", {"/a", "/b"}, 1, 2, &error);
  ASSERT_TRUE(ba.has_value()) << error;
  EXPECT_EQ(mosaico::descriptorIdentity(*ab), mosaico::descriptorIdentity(*ba));
  // The same validation bar as the parser: a scheme-carrying or port-less
  // origin is rejected.
  EXPECT_FALSE(mosaico::makePullDescriptor("grpc+tls://demo.mosaico.dev:6726", "seq", {"/a"}, 1, 2, &error));
  EXPECT_FALSE(mosaico::makePullDescriptor("demo.mosaico.dev", "seq", {"/a"}, 1, 2, &error));
  // Invalid UTF-8 is rejected, never U+FFFD-substituted: a substituted
  // descriptor would name a DIFFERENT request and collide with a genuine
  // replacement-char name.
  EXPECT_FALSE(mosaico::makePullDescriptor("demo.mosaico.dev:6726", "seq\xFF", {"/a"}, 1, 2, &error));
  EXPECT_NE(error.find("UTF-8"), std::string::npos) << error;
  EXPECT_FALSE(mosaico::makePullDescriptor("demo.mosaico.dev:6726", "seq", {"/a\xFF"}, 1, 2, &error));
}
