// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// live_server_harness — Task 0.1 diagnostic harness.
//
// A standalone, Qt-free executable that proves the live Mosaico download path
// works end-to-end and captures the real Arrow schema of an RGB image topic.
// It is the prerequisite that unblocks the rest of the image-MVP work.
//
// Steps (all printed to stdout):
//   1. Read MOSAICO_API_KEY from the environment. If absent, print
//      "SKIP: no MOSAICO_API_KEY" and return 0 (CI-safe).
//   2. Construct a MosaicoClient against grpc+tls://demo.mosaico.dev:6726
//      (one-way TLS, system root CA) with the API key.
//   3. listSequences() — print the count and the first few names. Pick the
//      bonirob sequence (or the first sequence containing an image-ish topic).
//   4. listTopics(sequence) — print every topic name + its ontology tag
//      (fetched lazily via getTopicMetadata, since listTopics leaves it empty).
//   5. Pick an RGB image topic (prefer camera/jai/rgb, camera/kinect/color, or
//      a name containing rgb/color/image). pullTopic over its full range.
//   6. For the FIRST record batch: print the COMPLETE Arrow schema, the row
//      count, ROW-0 values of the image columns, and the schema-level
//      KeyValueMetadata (mosaico:properties / ontology_tag).
//   7. Write the first batch to tests/fixtures/<slug>_batch.arrow (Arrow IPC
//      file format). Print the path + file size.
//
// This is a diagnostic, not a unit test: it links the SDK + Arrow only, no
// GTest and no Qt.

#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/ipc/writer.h>
#include <arrow/record_batch.h>
#include <arrow/scalar.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/util/key_value_metadata.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "flight/mosaico_client.hpp"
#include "flight/types.hpp"

namespace {

constexpr const char* kDemoUri = "grpc+tls://demo.mosaico.dev:6726";

// Replace path separators and anything non-alnum with '_' so the topic name
// becomes a safe single-segment filename.
std::string slugify(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const auto uc = static_cast<unsigned char>(c);
    out.push_back(std::isalnum(uc) != 0 ? static_cast<char>(std::tolower(uc)) : '_');
  }
  // Collapse runs of underscores for readability.
  std::string collapsed;
  collapsed.reserve(out.size());
  bool prev_us = false;
  for (char c : out) {
    if (c == '_') {
      if (!prev_us) {
        collapsed.push_back(c);
      }
      prev_us = true;
    } else {
      collapsed.push_back(c);
      prev_us = false;
    }
  }
  while (!collapsed.empty() && collapsed.back() == '_') {
    collapsed.pop_back();
  }
  return collapsed.empty() ? std::string("topic") : collapsed;
}

std::string lower(const std::string& s) {
  std::string out = s;
  std::transform(
      out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool isImageTag(const std::string& tag) {
  return tag == "image" || tag == "compressed_image";
}

// Score a topic for "is this an RGB image we want?". Higher is better; < 0 means
// not an image at all.
int rgbScore(const std::string& topic_name, const std::string& ontology_tag) {
  if (!isImageTag(ontology_tag)) {
    return -1;
  }
  const std::string n = lower(topic_name);
  int score = 0;
  if (n.find("camera/jai/rgb") != std::string::npos) {
    score += 100;
  }
  if (n.find("camera/kinect/color") != std::string::npos) {
    score += 90;
  }
  if (n.find("rgb") != std::string::npos) {
    score += 40;
  }
  if (n.find("color") != std::string::npos) {
    score += 35;
  }
  if (n.find("colour") != std::string::npos) {
    score += 35;
  }
  if (n.find("image") != std::string::npos) {
    score += 10;
  }
  // Penalise obviously-non-RGB image channels so they lose to a true RGB topic.
  if (n.find("nir") != std::string::npos || n.find("depth") != std::string::npos ||
      n.find("mono") != std::string::npos || n.find("ir") != std::string::npos) {
    score -= 5;
  }
  // Raw 'image' ontology preferred over 'compressed_image' for schema capture,
  // but only as a tie-breaker.
  if (ontology_tag == "image") {
    score += 1;
  }
  return score;
}

// Print the value of an int-like (any integer width / unsigned) column at row 0.
void printIntColumnRow0(const std::shared_ptr<arrow::RecordBatch>& batch, const char* name) {
  auto col = batch->GetColumnByName(name);
  if (!col) {
    std::cout << "      " << name << ": <absent>\n";
    return;
  }
  if (col->length() == 0 || col->IsNull(0)) {
    std::cout << "      " << name << ": <null> (type " << col->type()->ToString() << ")\n";
    return;
  }
  // Use the generic scalar accessor so we don't have to switch on every width.
  auto scalar_res = col->GetScalar(0);
  if (scalar_res.ok()) {
    std::cout << "      " << name << ": " << scalar_res.ValueOrDie()->ToString() << "  (type "
              << col->type()->ToString() << ")\n";
  } else {
    std::cout << "      " << name << ": <unreadable: " << scalar_res.status().ToString() << ">\n";
  }
}

void printStringColumnRow0(const std::shared_ptr<arrow::RecordBatch>& batch, const char* name) {
  auto col = batch->GetColumnByName(name);
  if (!col) {
    std::cout << "      " << name << ": <absent>\n";
    return;
  }
  if (col->length() == 0 || col->IsNull(0)) {
    std::cout << "      " << name << ": <null> (type " << col->type()->ToString() << ")\n";
    return;
  }
  auto scalar_res = col->GetScalar(0);
  if (scalar_res.ok()) {
    std::cout << "      " << name << ": " << scalar_res.ValueOrDie()->ToString() << "  (type "
              << col->type()->ToString() << ")\n";
  } else {
    std::cout << "      " << name << ": <unreadable: " << scalar_res.status().ToString() << ">\n";
  }
}

// Report the byte size of row 0 of a binary-like column.
void printBinaryColumnRow0Size(const std::shared_ptr<arrow::RecordBatch>& batch, const char* name) {
  auto col = batch->GetColumnByName(name);
  if (!col) {
    std::cout << "      " << name << " byte size: <absent>\n";
    return;
  }
  std::cout << "      " << name << " column type: " << col->type()->ToString() << ", length " << col->length() << "\n";
  if (col->length() == 0 || col->IsNull(0)) {
    std::cout << "      " << name << "[0] byte size: <null>\n";
    return;
  }
  int64_t nbytes = -1;
  switch (col->type()->id()) {
    case arrow::Type::BINARY: {
      auto* a = static_cast<arrow::BinaryArray*>(col.get());
      nbytes = a->value_length(0);
      break;
    }
    case arrow::Type::LARGE_BINARY: {
      auto* a = static_cast<arrow::LargeBinaryArray*>(col.get());
      nbytes = a->value_length(0);
      break;
    }
    case arrow::Type::BINARY_VIEW: {
      auto* a = static_cast<arrow::BinaryViewArray*>(col.get());
      nbytes = a->GetView(0).size();
      break;
    }
    case arrow::Type::FIXED_SIZE_BINARY: {
      auto* a = static_cast<arrow::FixedSizeBinaryArray*>(col.get());
      nbytes = a->byte_width();
      break;
    }
    case arrow::Type::STRING: {
      auto* a = static_cast<arrow::StringArray*>(col.get());
      nbytes = a->value_length(0);
      break;
    }
    case arrow::Type::LARGE_STRING: {
      auto* a = static_cast<arrow::LargeStringArray*>(col.get());
      nbytes = a->value_length(0);
      break;
    }
    default:
      break;
  }
  if (nbytes >= 0) {
    std::cout << "      " << name << "[0] byte size: " << nbytes << "\n";
  } else {
    std::cout << "      " << name << "[0] byte size: <unknown for type " << col->type()->ToString() << ">\n";
  }
}

void printSchema(const std::shared_ptr<arrow::Schema>& schema) {
  std::cout << "  --- COMPLETE Arrow schema (" << schema->num_fields() << " fields) ---\n";
  for (int i = 0; i < schema->num_fields(); ++i) {
    const auto& f = schema->field(i);
    std::cout << "    [" << i << "] " << f->name() << " : " << f->type()->ToString()
              << (f->nullable() ? " (nullable)" : " (not null)") << "\n";
  }
  std::cout << "  --- schema-level KeyValueMetadata ---\n";
  const auto& md = schema->metadata();
  if (!md) {
    std::cout << "    <none>\n";
    return;
  }
  for (int64_t i = 0; i < md->size(); ++i) {
    std::cout << "    " << md->key(i) << " = " << md->value(i) << "\n";
  }
}

int run(const std::string& api_key) {
  std::cout << "[harness] connecting to " << kDemoUri << " (TLS, system root CA)\n";
  mosaico::MosaicoClient client(
      kDemoUri,
      /*timeout_seconds=*/30,
      /*pool_size=*/4,
      /*tls_cert_path=*/std::string(),
      /*api_key=*/api_key);

  auto version = client.version();
  if (!version.ok()) {
    std::cerr << "[harness] ERROR: version() failed: " << version.status().ToString() << "\n";
    return 1;
  }
  std::cout << "[harness] connected — server version " << version.ValueOrDie().version << "\n";

  // --- listSequences -------------------------------------------------------
  auto sequences = client.listSequences(nullptr, nullptr);
  if (!sequences.ok()) {
    std::cerr << "[harness] ERROR: listSequences failed: " << sequences.status().ToString() << "\n";
    return 1;
  }
  const auto& seqs = sequences.ValueOrDie();
  std::cout << "[harness] listSequences -> " << seqs.size() << " sequences\n";
  for (size_t i = 0; i < seqs.size() && i < 10; ++i) {
    std::cout << "    seq[" << i << "] " << seqs[i].name << "  (ts ns [" << seqs[i].min_ts_ns << ", "
              << seqs[i].max_ts_ns << "], " << seqs[i].total_size_bytes << " bytes)\n";
  }
  if (seqs.empty()) {
    std::cerr << "[harness] ERROR: server returned no sequences\n";
    return 1;
  }

  // Prefer a sequence whose name contains "bonirob" or "ijrr". Otherwise we
  // fall back to scanning each sequence for an image topic.
  std::vector<size_t> seq_order;
  for (size_t i = 0; i < seqs.size(); ++i) {
    const std::string n = lower(seqs[i].name);
    if (n.find("bonirob") != std::string::npos || n.find("ijrr") != std::string::npos) {
      seq_order.push_back(i);
    }
  }
  for (size_t i = 0; i < seqs.size(); ++i) {
    if (std::find(seq_order.begin(), seq_order.end(), i) == seq_order.end()) {
      seq_order.push_back(i);
    }
  }

  // --- find best RGB image topic across the prioritised sequences ----------
  std::string chosen_seq;
  std::string chosen_topic;
  std::string chosen_ontology;
  int64_t chosen_min_ts = 0;
  int64_t chosen_max_ts = 0;
  int best_score = -1;

  // First sequence we successfully list topics for: we print its full topic
  // list with ontology tags (the task asks for the topic list of the bonirob
  // sequence). Capture it so we can print it even if the RGB topic comes from
  // there (which it should).
  bool printed_topic_list = false;

  int sequences_scanned = 0;
  for (size_t idx : seq_order) {
    const auto& seq = seqs[idx];
    auto topics = client.listTopics(seq.name);
    if (!topics.ok()) {
      std::cout << "[harness] listTopics(" << seq.name << ") failed: " << topics.status().ToString() << " — skipping\n";
      continue;
    }
    ++sequences_scanned;
    const auto& topic_stubs = topics.ValueOrDie();

    std::cout << "[harness] sequence '" << seq.name << "' has " << topic_stubs.size() << " topics:\n";

    bool found_image_here = false;
    for (const auto& stub : topic_stubs) {
      // listTopics leaves ontology_tag empty — fetch it on demand.
      std::string ontology;
      auto meta = client.getTopicMetadata(seq.name, stub.topic_name);
      if (meta.ok()) {
        ontology = meta.ValueOrDie().ontology_tag;
      }
      std::cout << "    topic '" << stub.topic_name << "'  ontology='" << (ontology.empty() ? "<empty>" : ontology)
                << "'  ts ns [" << stub.min_ts_ns << ", " << stub.max_ts_ns << "]\n";

      const int score = rgbScore(stub.topic_name, ontology);
      if (score >= 0) {
        found_image_here = true;
      }
      if (score > best_score) {
        best_score = score;
        chosen_seq = seq.name;
        chosen_topic = stub.topic_name;
        chosen_ontology = ontology;
        chosen_min_ts = stub.min_ts_ns;
        chosen_max_ts = stub.max_ts_ns;
      }
    }
    printed_topic_list = true;

    // If this sequence already gave us an RGB-ish hit, stop scanning more
    // sequences — we have what we need and avoid hammering the server.
    if (found_image_here && best_score >= 40) {
      break;
    }
    // Bound the scan if no good hit yet.
    if (sequences_scanned >= 8) {
      break;
    }
  }

  if (!printed_topic_list) {
    std::cerr << "[harness] ERROR: could not list topics for any sequence\n";
    return 1;
  }
  if (chosen_topic.empty() || best_score < 0) {
    std::cerr << "[harness] ERROR: no image/compressed_image topic found on the server\n";
    return 1;
  }

  std::cout << "[harness] chosen RGB topic: sequence='" << chosen_seq << "' topic='" << chosen_topic << "' ontology='"
            << chosen_ontology << "' (score " << best_score << ")\n";

  // --- pull the chosen topic over its full time range ----------------------
  // Capture the first batch + schema via callbacks so we don't depend on the
  // whole pull completing before we can inspect it. We still let pullTopic run
  // to completion (it returns the full PullResult).
  std::shared_ptr<arrow::RecordBatch> first_batch;
  std::shared_ptr<arrow::Schema> stream_schema;
  std::atomic<bool> cancel{false};

  auto schema_cb = [&](const std::shared_ptr<arrow::Schema>& s) {
    if (!stream_schema) {
      stream_schema = s;
    }
  };
  auto batch_cb = [&](const std::shared_ptr<arrow::RecordBatch>& b) {
    if (!first_batch && b && b->num_rows() > 0) {
      first_batch = b;
      // We have the first batch — cancel the rest of the (potentially large)
      // stream so the harness stays fast. retain_batches=false avoids holding
      // every batch in memory.
      cancel = true;
    }
  };

  mosaico::TimeRange range;  // full range
  range.start_ns = chosen_min_ts > 0 ? std::optional<int64_t>(chosen_min_ts) : std::nullopt;
  range.end_ns = chosen_max_ts > chosen_min_ts ? std::optional<int64_t>(chosen_max_ts) : std::nullopt;

  std::cout << "[harness] pulling topic over range ns [" << range.start_ns.value_or(0) << ", "
            << range.end_ns.value_or(0) << "] (cancel after first batch)\n";

  auto result = client.pullTopic(
      chosen_seq, chosen_topic, range,
      /*progress_cb=*/nullptr, &cancel, batch_cb, schema_cb,
      /*retain_batches=*/false);

  // A pull cancelled after the first batch returns Cancelled — that's expected
  // and fine as long as we captured a batch.
  if (!result.ok() && !first_batch) {
    std::cerr << "[harness] ERROR: pullTopic failed before any batch: " << result.status().ToString() << "\n";
    return 1;
  }
  if (result.ok() && !first_batch) {
    // No callback batch captured — fall back to the returned PullResult.
    const auto& pull = result.ValueOrDie();
    if (!pull.batches.empty()) {
      first_batch = pull.batches.front();
    }
    if (!stream_schema) {
      stream_schema = pull.schema;
    }
  }
  if (!first_batch) {
    std::cerr << "[harness] ERROR: pull returned no non-empty batches\n";
    return 1;
  }

  const auto schema = stream_schema ? stream_schema : first_batch->schema();

  std::cout << "\n[harness] ===== FIRST RECORD BATCH =====\n";
  std::cout << "  row count: " << first_batch->num_rows() << "\n";
  printSchema(schema);

  std::cout << "  --- ROW 0 image-relevant values ---\n";
  printStringColumnRow0(first_batch, "format");
  printStringColumnRow0(first_batch, "encoding");
  printIntColumnRow0(first_batch, "width");
  printIntColumnRow0(first_batch, "height");
  printIntColumnRow0(first_batch, "stride");
  printIntColumnRow0(first_batch, "step");
  printIntColumnRow0(first_batch, "row_step");
  printIntColumnRow0(first_batch, "is_bigendian");
  printIntColumnRow0(first_batch, "timestamp_ns");
  printIntColumnRow0(first_batch, "recording_timestamp_ns");
  printBinaryColumnRow0Size(first_batch, "data");

  // --- write the first batch to an Arrow IPC file fixture ------------------
  namespace fs = std::filesystem;
  const fs::path fixtures_dir = fs::path(__FILE__).parent_path() / "fixtures";  // tests/fixtures
  std::error_code ec;
  fs::create_directories(fixtures_dir, ec);
  if (ec) {
    std::cerr << "[harness] ERROR: could not create fixtures dir " << fixtures_dir << ": " << ec.message() << "\n";
    return 1;
  }
  const std::string slug = slugify(chosen_seq + "_" + chosen_topic);
  const fs::path fixture_path = fixtures_dir / (slug + "_batch.arrow");

  auto sink_res = arrow::io::FileOutputStream::Open(fixture_path.string());
  if (!sink_res.ok()) {
    std::cerr << "[harness] ERROR: cannot open fixture for write: " << sink_res.status().ToString() << "\n";
    return 1;
  }
  auto sink = sink_res.ValueOrDie();
  auto writer_res = arrow::ipc::MakeFileWriter(sink, schema);
  if (!writer_res.ok()) {
    std::cerr << "[harness] ERROR: MakeFileWriter failed: " << writer_res.status().ToString() << "\n";
    return 1;
  }
  auto writer = writer_res.ValueOrDie();
  if (auto st = writer->WriteRecordBatch(*first_batch); !st.ok()) {
    std::cerr << "[harness] ERROR: WriteRecordBatch failed: " << st.ToString() << "\n";
    return 1;
  }
  if (auto st = writer->Close(); !st.ok()) {
    std::cerr << "[harness] ERROR: writer Close failed: " << st.ToString() << "\n";
    return 1;
  }
  if (auto st = sink->Close(); !st.ok()) {
    std::cerr << "[harness] ERROR: sink Close failed: " << st.ToString() << "\n";
    return 1;
  }

  const auto file_size = fs::file_size(fixture_path, ec);
  std::cout << "\n[harness] wrote fixture: " << fixture_path.string() << " ("
            << (ec ? -1 : static_cast<int64_t>(file_size)) << " bytes)\n";

  std::cout << "[harness] PASS — live download works; schema + fixture captured.\n";
  return 0;
}

}  // namespace

int main() {
  const char* api_key_env = std::getenv("MOSAICO_API_KEY");
  const std::string api_key =
      (api_key_env != nullptr && api_key_env[0] != '\0') ? std::string(api_key_env) : std::string();
  if (api_key.empty()) {
    std::cout << "SKIP: no MOSAICO_API_KEY\n";
    return 0;
  }
  return run(api_key);
}
