/**
 * @file mcap_dataset_metadata_test.cpp
 * @brief Tests for the dataset-metadata extraction (mcap_dataset_metadata.hpp).
 *
 * Fixtures are real MCAP files written to a temp path with mcap::McapWriter,
 * then read back through McapReader — the same reader machinery importData()
 * uses. Covers: PJ capture+recording records present (descriptor recovered
 * from the pj.source.v1 identity framing), repeated generic metadata names,
 * files with no metadata at all, and a malformed pj.capture manifest
 * (diagnostic, extraction still succeeds).
 */

#define MCAP_IMPLEMENTATION
#include "../mcap_dataset_metadata.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <mcap/writer.hpp>
#include <random>
#include <string>
#include <vector>

#include "pj_base/sdk/data_source_host_views.hpp"

namespace {

using PJ::McapMetadata::parseIdentityFraming;

// RAII temp MCAP path; the writer needs a real file for the reader's
// positioned-read data source.
struct TempMcap {
  std::string path;

  TempMcap() {
    path = (std::filesystem::temp_directory_path() /
            ("pj_dataset_metadata_test_" + std::to_string(run_token()) + "_" + std::to_string(counter_++) + ".mcap"))
               .string();
  }
  ~TempMcap() {
    std::remove(path.c_str());
  }

  // Per-process uniqueness without ::getpid (POSIX-only spelling breaks MSVC).
  static unsigned run_token() {
    static const unsigned token = std::random_device{}();
    return token;
  }

  static inline int counter_ = 0;
};

mcap::Metadata makeRecord(std::string name, std::initializer_list<std::pair<std::string, std::string>> entries) {
  mcap::Metadata record;
  record.name = std::move(name);
  for (auto& [key, value] : entries) {
    record.metadata[key] = value;
  }
  return record;
}

// Writes one message on one channel plus the given metadata records.
void writeFixture(const std::string& path, const std::vector<mcap::Metadata>& records) {
  mcap::McapWriter writer;
  mcap::McapWriterOptions options("");
  ASSERT_TRUE(writer.open(path, options).ok());

  mcap::Schema schema("test_schema", "jsonschema", "{}");
  writer.addSchema(schema);
  mcap::Channel channel("/topic", "json", schema.id);
  writer.addChannel(channel);

  const std::string payload = "{\"value\":1}";
  mcap::Message message;
  message.channelId = channel.id;
  message.logTime = 1000;
  message.publishTime = 1000;
  message.data = reinterpret_cast<const std::byte*>(payload.data());
  message.dataSize = payload.size();
  ASSERT_TRUE(writer.write(message).ok());

  for (const mcap::Metadata& record : records) {
    mcap::Metadata copy = record;
    ASSERT_TRUE(writer.write(copy).ok());
  }
  writer.close();
}

nlohmann::json extractFrom(const std::string& path, std::vector<std::string>* diagnostics) {
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(path).ok());
  EXPECT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());
  return PJ::McapMetadata::extractDatasetMetadata(reader, diagnostics);
}

TEST(ParseIdentityFraming, RoundTripsAndRejects) {
  const auto parts = parseIdentityFraming("pj.source.v1|10|mcap_cloud|{\"kind\":\"x\"}");
  ASSERT_TRUE(parts.has_value());
  EXPECT_EQ(parts->provider_id, "mcap_cloud");
  EXPECT_EQ(parts->descriptor_json, "{\"kind\":\"x\"}");

  EXPECT_FALSE(parseIdentityFraming("pj.source.v2|10|mcap_cloud|{}").has_value());
  EXPECT_FALSE(parseIdentityFraming("pj.source.v1|999|short|{}").has_value());
  EXPECT_FALSE(parseIdentityFraming("pj.source.v1|abc|x|{}").has_value());
  EXPECT_FALSE(parseIdentityFraming("pj.source.v1|4|mcap{}").has_value());
  EXPECT_FALSE(parseIdentityFraming("").has_value());
}

TEST(McapDatasetMetadata, CaptureAndRecordingParsed) {
  const std::string descriptor = R"({"kind":"mosaico.pull","v":1,"request":{"origin":"demo.example.org:6726"}})";
  const std::string identity = "pj.source.v1|10|mcap_cloud|" + descriptor;
  nlohmann::json manifest;
  manifest["version"] = 1;
  manifest["provider_id"] = "mcap_cloud";
  manifest["identity"] = identity;
  manifest["total_messages"] = 42;

  TempMcap fixture;
  writeFixture(
      fixture.path, {
                        makeRecord("pj.recording", {{"version", "1"}, {"started_utc", "2026-09-06T10:00:00Z"}}),
                        makeRecord("note", {{"author", "alice"}}),
                        makeRecord("note", {{"author", "bob"}}),
                        makeRecord(
                            "pj.recording", {{"version", "1"},
                                             {"started_utc", "2026-09-06T10:00:00Z"},
                                             {"stopped_utc", "2026-09-06T10:05:00Z"},
                                             {"terminal_cause", "stopped"},
                                             {"messages", "42"}}),
                        makeRecord("pj.capture", {{"json", manifest.dump()}}),
                    });

  std::vector<std::string> diagnostics;
  const nlohmann::json document = extractFrom(fixture.path, &diagnostics);
  EXPECT_TRUE(diagnostics.empty()) << diagnostics.front();

  // File facts from the summary statistics.
  EXPECT_EQ(document["file"]["message_count"], 1);
  EXPECT_EQ(document["file"]["channel_count"], 1);
  EXPECT_EQ(document["file"]["message_start_time_ns"], 1000);

  // All five records survive in file order; repeated names stay distinct.
  const auto& raw_records = document["mcap_metadata"];
  ASSERT_EQ(raw_records.size(), 5);
  EXPECT_EQ(raw_records[1]["name"], "note");
  EXPECT_EQ(raw_records[1]["entries"]["author"], "alice");
  EXPECT_EQ(raw_records[2]["entries"]["author"], "bob");

  // The recording branch is the CLOSING pj.recording record.
  EXPECT_EQ(document["recording"]["terminal_cause"], "stopped");
  EXPECT_EQ(document["recording"]["stopped_utc"], "2026-09-06T10:05:00Z");

  // The capture branch carries the manifest with the descriptor recovered
  // from the identity framing, raw identity retained.
  EXPECT_EQ(document["capture"]["provider_id"], "mcap_cloud");
  EXPECT_EQ(document["capture"]["identity"], identity);
  EXPECT_EQ(document["capture"]["descriptor"]["kind"], "mosaico.pull");
  EXPECT_EQ(document["capture"]["descriptor"]["request"]["origin"], "demo.example.org:6726");
}

TEST(McapDatasetMetadata, FileWithoutMetadataRecords) {
  TempMcap fixture;
  writeFixture(fixture.path, {});

  std::vector<std::string> diagnostics;
  const nlohmann::json document = extractFrom(fixture.path, &diagnostics);
  EXPECT_TRUE(diagnostics.empty());

  EXPECT_EQ(document["file"]["message_count"], 1);
  EXPECT_FALSE(document.contains("mcap_metadata"));
  EXPECT_FALSE(document.contains("capture"));
  EXPECT_FALSE(document.contains("recording"));
}

TEST(McapDatasetMetadata, MalformedCaptureManifestIsDiagnosedNotFatal) {
  TempMcap fixture;
  writeFixture(
      fixture.path, {
                        makeRecord("pj.capture", {{"json", "{not json"}}),
                        makeRecord("pj.capture", {{"wrong_key", "x"}}),
                    });

  std::vector<std::string> diagnostics;
  const nlohmann::json document = extractFrom(fixture.path, &diagnostics);

  // Extraction succeeded: the raw records are present, the parsed branch is
  // absent, and the problem surfaced as a diagnostic.
  ASSERT_EQ(document["mcap_metadata"].size(), 2);
  EXPECT_FALSE(document.contains("capture"));
  EXPECT_FALSE(diagnostics.empty());
}

TEST(McapDatasetMetadata, OversizedRecordIsElided) {
  TempMcap fixture;
  writeFixture(fixture.path, {makeRecord("huge", {{"blob", std::string(PJ::McapMetadata::kMaxRecordBytes + 1, 'x')}})});

  std::vector<std::string> diagnostics;
  const nlohmann::json document = extractFrom(fixture.path, &diagnostics);

  ASSERT_EQ(document["mcap_metadata"].size(), 1);
  EXPECT_FALSE(document["mcap_metadata"][0].contains("entries"));
  EXPECT_TRUE(document["mcap_metadata"][0].contains("skipped"));
  EXPECT_FALSE(diagnostics.empty());
}

// The manifest's sdk_floor_exceptions entry for setDatasetMetadata names this
// test: on a host that predates the slot (short struct_size), publishing the
// extracted document must be a clean no-op — the import path is unaffected and
// only the metadata display is missing.
TEST(McapDatasetMetadata, ImportSucceedsAgainstFloorLevelHost) {
  PJ_data_source_runtime_host_vtable_t vtable{};
  vtable.protocol_version = 1;
  vtable.struct_size = offsetof(PJ_data_source_runtime_host_vtable_t, set_dataset_metadata);
  PJ_data_source_runtime_host_t host{};
  int host_context = 0;
  host.ctx = &host_context;
  host.vtable = &vtable;
  const PJ::DataSourceRuntimeHostView view(host);

  const nlohmann::json document = {{"file", {{"message_count", 1}}}};
  PJ::McapMetadata::publishDatasetMetadata(view, document);  // must not throw or call anything
  SUCCEED();
}

}  // namespace
