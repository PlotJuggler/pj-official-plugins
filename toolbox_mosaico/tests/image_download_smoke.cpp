// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// image_download_smoke — network-dependent smoke that exercises the
// real Mosaico Image MVP path end-to-end against demo.mosaico.dev:
//
//   1. Connect via MosaicoClient (TLS).
//   2. listSequences -> pick the first non-empty sequence.
//   3. listTopics on that sequence -> find a topic whose ontology_tag
//      is "image" or "compressed_image".
//   4. pullTopic on that image topic (bounded to a small time window
//      so the smoke runs in seconds, not minutes).
//   5. Assert: the returned PullResult has at least one RecordBatch
//      with rows; the schema includes the expected image columns
//      (data + format + at least one of width/height).
//
// The test is **opt-in** behind GTEST_FILTER or by setting the env var
// MOSAICO_LIVE_SMOKE=1 — CI typically skips it. Cancel flag wired so
// the test exits promptly if the server is unreachable.

#include <arrow/api.h>
#include <arrow/c/abi.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "flight/mosaico_client.hpp"
#include "flight/types.hpp"

namespace {

constexpr const char* kDemoUri = "grpc+tls://demo.mosaico.dev:6726";

bool liveSmokeRequested() {
  const char* env = std::getenv("MOSAICO_LIVE_SMOKE");
  return env != nullptr && env[0] != '\0' && std::string(env) != "0";
}

}  // namespace

TEST(MosaicoImageDownloadSmoke, FetchSequenceWithImageTopic) {
  if (!liveSmokeRequested()) {
    GTEST_SKIP() << "Set MOSAICO_LIVE_SMOKE=1 to run this network-dependent test";
  }

  std::cerr << "[smoke] connecting to " << kDemoUri << "\n";
  const char* api_key_env = std::getenv("MOSAICO_API_KEY");
  std::string api_key = (api_key_env != nullptr && api_key_env[0] != '\0') ? api_key_env : std::string();
  if (api_key.empty()) {
    GTEST_SKIP() << "Set MOSAICO_API_KEY to authenticate against the demo server";
  }
  mosaico::MosaicoClient client(
      kDemoUri,
      /*timeout_seconds=*/15,
      /*pool_size=*/2,
      /*cert_path=*/std::string(),
      /*api_key=*/api_key);

  auto version = client.version();
  ASSERT_TRUE(version.ok()) << "version() failed: " << version.status().ToString();
  std::cerr << "[smoke] connected — server version " << version.ValueOrDie().version << "\n";

  auto sequences = client.listSequences(nullptr, nullptr);
  ASSERT_TRUE(sequences.ok()) << "listSequences failed: " << sequences.status().ToString();
  ASSERT_FALSE(sequences.ValueOrDie().empty()) << "demo server returned no sequences";
  std::cerr << "[smoke] got " << sequences.ValueOrDie().size() << " sequences\n";

  // Walk sequences until we find one with an image-tagged topic.
  std::string target_sequence;
  std::string target_topic;
  std::string target_ontology;
  int64_t topic_min_ts = 0;
  int64_t topic_max_ts = 0;

  // listTopics returns lightweight TopicInfo entries — schema, ontology_tag,
  // and user_metadata are EMPTY by design and must be fetched on demand
  // via getTopicMetadata. Walk sequences, then for each topic fetch the
  // full metadata to inspect ontology_tag.
  std::map<std::string, int> ontology_counts;
  int sequences_walked = 0;
  int topics_inspected = 0;
  for (const auto& seq : sequences.ValueOrDie()) {
    auto topics = client.listTopics(seq.name);
    ++sequences_walked;
    if (!topics.ok()) {
      continue;
    }
    for (const auto& topic_stub : topics.ValueOrDie()) {
      auto info_result = client.getTopicMetadata(seq.name, topic_stub.topic_name);
      ++topics_inspected;
      if (!info_result.ok()) {
        continue;
      }
      const auto& info = info_result.ValueOrDie();
      ++ontology_counts[info.ontology_tag];
      if (info.ontology_tag == "image" || info.ontology_tag == "compressed_image") {
        target_sequence = seq.name;
        target_topic = topic_stub.topic_name;
        target_ontology = info.ontology_tag;
        topic_min_ts = topic_stub.min_ts_ns;
        topic_max_ts = topic_stub.max_ts_ns;
        break;
      }
    }
    if (!target_topic.empty()) {
      break;
    }
    // Cap the walk so the smoke runs in a bounded time when no image
    // topics exist on the server.
    if (sequences_walked >= 20 || topics_inspected >= 80) {
      break;
    }
  }
  std::cerr << "[smoke] inspected " << topics_inspected << " topics across " << sequences_walked << " sequences\n";

  std::cerr << "[smoke] ontology tags seen across " << sequences_walked << " sequences:\n";
  for (const auto& [tag, n] : ontology_counts) {
    std::cerr << "[smoke]   " << (tag.empty() ? "<empty>" : tag) << " x " << n << "\n";
  }

  if (target_topic.empty()) {
    GTEST_SKIP() << "No image/compressed_image topics found on the demo server right now";
  }

  std::cerr << "[smoke] target sequence=" << target_sequence << " topic=" << target_topic
            << " ontology=" << target_ontology << "\n";

  // Bound the pull to the first 1 second of data so the smoke runs in seconds.
  mosaico::TimeRange range;
  range.start_ns = topic_min_ts;
  if (topic_max_ts > topic_min_ts) {
    range.end_ns = topic_min_ts + std::min<int64_t>(1'000'000'000LL, topic_max_ts - topic_min_ts);
  }

  std::atomic<bool> cancel{false};
  std::cerr << "[smoke] pulling topic [" << range.start_ns.value_or(0) << ", " << range.end_ns.value_or(0) << "]\n";
  auto result = client.pullTopic(
      target_sequence, target_topic, range,
      /*progress_cb=*/nullptr, &cancel);
  ASSERT_TRUE(result.ok()) << "pullTopic failed: " << result.status().ToString();

  const auto& pull = result.ValueOrDie();
  ASSERT_NE(pull.schema, nullptr) << "pullTopic returned null schema";
  ASSERT_FALSE(pull.batches.empty()) << "pullTopic returned no batches";

  int64_t total_rows = 0;
  for (const auto& batch : pull.batches) {
    if (batch) {
      total_rows += batch->num_rows();
    }
  }
  std::cerr << "[smoke] got " << pull.batches.size() << " batches, " << total_rows << " rows, "
            << pull.decoded_size_bytes << " bytes decoded\n";
  EXPECT_GT(total_rows, 0);

  // Validate the schema looks like a Mosaico image: must have a binary
  // `data` field, plus either width/height (raw image) or format
  // (compressed image). Recent Arrow uses BINARY_VIEW for the canonical
  // binary column type; accept all binary-ish variants.
  const auto& schema = pull.schema;
  auto data_field = schema->GetFieldByName("data");
  ASSERT_NE(data_field, nullptr) << "image schema must have a 'data' column";
  const auto data_type_id = data_field->type()->id();
  const bool data_is_binary_like = data_type_id == arrow::Type::BINARY || data_type_id == arrow::Type::LARGE_BINARY ||
                                   data_type_id == arrow::Type::BINARY_VIEW ||
                                   data_type_id == arrow::Type::FIXED_SIZE_BINARY;
  EXPECT_TRUE(data_is_binary_like) << "image 'data' column expected to be a binary type, got "
                                   << data_field->type()->ToString();

  if (target_ontology == "image") {
    EXPECT_NE(schema->GetFieldByName("width"), nullptr) << "raw image schema must have 'width'";
    EXPECT_NE(schema->GetFieldByName("height"), nullptr) << "raw image schema must have 'height'";
  } else {  // compressed_image
    EXPECT_NE(schema->GetFieldByName("format"), nullptr) << "compressed_image schema must have 'format'";
  }

  // Sample the first row's data column and assert it's non-empty.
  ASSERT_FALSE(pull.batches.empty());
  const auto& first_batch = pull.batches.front();
  auto data_col = first_batch->GetColumnByName("data");
  ASSERT_NE(data_col, nullptr);
  ASSERT_GT(data_col->length(), 0);
  std::cerr << "[smoke] first row 'data' column type=" << data_col->type()->ToString()
            << " length=" << data_col->length() << "\n";

  std::cerr << "[smoke] PASS — downloaded sequence containing image data\n";
}
