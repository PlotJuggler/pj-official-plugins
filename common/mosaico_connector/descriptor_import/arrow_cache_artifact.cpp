// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// This is the single TU that instantiates the vendored header-only mcap
// implementation for Mosaico descriptor import.
#define MCAP_IMPLEMENTATION
#include "descriptor_import/arrow_cache_artifact.hpp"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mcap/reader.hpp>
#include <mcap/writer.hpp>
#include <pj_base/sdk/platform.hpp>

#include "descriptor_import/source_descriptor.hpp"

namespace mosaico {

namespace fs = std::filesystem;

namespace {

constexpr const char* kSchemaEncoding = "arrow_ipc_schema";
constexpr const char* kMessageEncoding = "arrow_ipc";
// Object channels: one message per row, payload = the canonical blob exactly
// as pushed to the ObjectStore. schemaId 0 (no schema record).
constexpr const char* kObjectMessageEncoding = "pj_canonical";
// Channel-metadata keys for the stored routing decision.
constexpr const char* kMetaOntologyTag = "mosaico:ontology_tag";
constexpr const char* kMetaCanonicalMetadata = "mosaico:canonical_metadata";
constexpr const char* kMetaTimestampColumn = "mosaico:timestamp_column";

// Bounded-query budgets (the validator runs on the GUI thread): a forged
// artifact must be refused from its raw footer BEFORE the MCAP parser
// allocates according to file-controlled sizes. Legit summaries are far
// below these bounds; descriptors are <= 64 KiB by the parse limit.
constexpr std::uintmax_t kQuerySummaryBudget = 16ull * 1024 * 1024;
constexpr std::uint64_t kQueryMetadataBudget = 1ull * 1024 * 1024;
// MCAP file tail: Footer record (1-byte op 0x02 + 8-byte length + 20-byte
// payload {summary_start, summary_offset_start, summary_crc}) + 8-byte magic.
constexpr std::uintmax_t kMcapTailBytes = 29 + 8;

bool summarySpanWithinBudget(const fs::path& path, std::string* error) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec || size < kMcapTailBytes + 8) {
    *error = "not a readable artifact: too small";
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "not a readable artifact: open failed";
    return false;
  }
  unsigned char tail[kMcapTailBytes];
  in.seekg(static_cast<std::streamoff>(size - kMcapTailBytes));
  in.read(reinterpret_cast<char*>(tail), sizeof(tail));
  if (!in) {
    *error = "not a readable artifact: footer unreadable";
    return false;
  }
  if (tail[0] != 0x02) {
    *error = "not a readable artifact: footer opcode mismatch";
    return false;
  }
  std::uint64_t summary_start = 0;
  for (int i = 0; i < 8; ++i) {
    summary_start |= static_cast<std::uint64_t>(tail[9 + i]) << (8 * i);
  }
  if (summary_start == 0) {
    return true;  // no summary — the reader will reject it as unreadable
  }
  const std::uintmax_t footer_offset = size - kMcapTailBytes;
  if (summary_start > footer_offset) {
    *error = "forged summary offset (past the footer)";
    return false;
  }
  const std::uintmax_t span = footer_offset - summary_start;
  if (span > kQuerySummaryBudget) {
    *error = "summary section exceeds the bounded query budget (" + std::to_string(span) + " bytes)";
    return false;
  }
  return true;
}

// Read one Metadata record named `name` through the bounded raw preflight:
// the MetadataIndex length cap alone is bypassable (ReadRecord re-reads the
// declared record size FROM THE FILE at the attacker-controlled offset and
// resizes its buffer to that value), so the 9-byte record header is raw-read
// and validated against both the index claim and the budget BEFORE the
// parser allocates anything.
bool readBoundedMetadata(
    mcap::McapReader& reader, const fs::path& path, const char* name, mcap::KeyValueMap* out, std::string* error) {
  const auto index = reader.metadataIndexes().find(name);
  if (index == reader.metadataIndexes().end()) {
    *error = std::string("missing embedded record (") + name + ")";
    return false;
  }
  if (index->second.length > kQueryMetadataBudget) {
    *error = std::string(name) + " record exceeds the bounded query budget";
    return false;
  }
  {
    std::error_code size_ec;
    const std::uintmax_t file_bytes = fs::file_size(path, size_ec);
    unsigned char header[9] = {0};
    std::ifstream raw(path, std::ios::binary);
    bool header_ok = !size_ec && raw.is_open() && index->second.offset + sizeof(header) <= file_bytes;
    if (header_ok) {
      raw.seekg(static_cast<std::streamoff>(index->second.offset));
      raw.read(reinterpret_cast<char*>(header), sizeof(header));
      header_ok = static_cast<bool>(raw);
    }
    std::uint64_t declared = 0;
    for (int i = 0; i < 8; ++i) {
      declared |= static_cast<std::uint64_t>(header[1 + i]) << (8 * i);
    }
    // Order matters: the budget check runs before any +9 arithmetic so a
    // huge declared size can never overflow a later comparison.
    if (!header_ok || header[0] != static_cast<unsigned char>(mcap::OpCode::Metadata) ||
        declared > kQueryMetadataBudget || declared + 9 > index->second.length ||
        index->second.offset + 9 + declared > file_bytes) {
      *error = std::string(name) + " record failed the bounded raw preflight";
      return false;
    }
  }
  mcap::Record record;
  auto status = mcap::McapReader::ReadRecord(*reader.dataSource(), index->second.offset, &record);
  if (!status.ok()) {
    *error = std::string(name) + " record unreadable: " + status.message;
    return false;
  }
  mcap::Metadata metadata;
  status = mcap::McapReader::ParseMetadata(record, &metadata);
  if (!status.ok()) {
    *error = std::string(name) + " record unparsable: " + status.message;
    return false;
  }
  *out = std::move(metadata.metadata);
  return true;
}

// The three channel-metadata entries every artifact channel carries (the
// ingest ROUTING DECISION, replayed verbatim) — one authority for both the
// scalar and the object channel kind.
mcap::KeyValueMap channelMetadataFor(const ArtifactTopic& topic) {
  return {
      {kMetaOntologyTag, topic.ontology_tag},
      {kMetaCanonicalMetadata, topic.canonical_metadata},
      {kMetaTimestampColumn, topic.timestamp_column}};
}

// One MCAP message per write, framed identically for both channel kinds
// (sequence 0; publishTime == logTime). `what` labels the error.
bool writeMcapMessage(
    mcap::McapWriter& writer, std::uint16_t channel_id, std::int64_t log_time_ns, const std::byte* data,
    std::uint64_t size, const char* what, std::string* error) {
  mcap::Message message;
  message.channelId = channel_id;
  message.sequence = 0;
  message.logTime = static_cast<mcap::Timestamp>(log_time_ns);
  message.publishTime = message.logTime;
  message.dataSize = size;
  message.data = data;
  const auto status = writer.write(message);
  if (!status.ok()) {
    if (error) {
      *error = std::string(what) + " failed: " + status.message;
    }
    return false;
  }
  return true;
}

}  // namespace

fs::path standardCacheRoot(std::string* error) {
  if (auto value = PJ::sdk::getEnv("MOSAICO_CACHE_DIR")) {
    return fs::path(*value);
  }
  if (auto value = PJ::sdk::getEnv("XDG_CACHE_HOME")) {
    return fs::path(*value) / "mosaico" / "sessions";
  }
  if (auto value = PJ::sdk::getEnv("HOME")) {
    return fs::path(*value) / ".cache" / "mosaico" / "sessions";
  }
  if (error != nullptr) {
    *error = "cache root unresolvable (MOSAICO_CACHE_DIR, XDG_CACHE_HOME and HOME all unset)";
  }
  return {};
}

PJ::sdk::descriptor_import::RequestArtifactCache makeArtifactCache(
    const fs::path& configured_root, std::string* error) {
  const fs::path root = configured_root.empty() ? standardCacheRoot(error) : configured_root;
  return PJ::sdk::descriptor_import::RequestArtifactCache(
      PJ::sdk::descriptor_import::CacheSpec{root, ".pjmosaico", sourceDescriptorPolicy().identity}, validateArtifact);
}

PJ::sdk::descriptor_import::CleanupPolicy cacheCleanupPolicy(double max_gb) {
  PJ::sdk::descriptor_import::CleanupPolicy policy;  // default: unlimited
  if (!std::isfinite(max_gb) || max_gb <= 0.0) {
    return policy;
  }
  constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
  constexpr double kMaxBytes = static_cast<double>(std::numeric_limits<std::uintmax_t>::max());
  const double bytes = max_gb * kGiB;
  policy.max_total_bytes =
      bytes >= kMaxBytes ? std::numeric_limits<std::uintmax_t>::max() : static_cast<std::uintmax_t>(bytes);
  return policy;
}

PJ::sdk::descriptor_import::CleanupResult maintainCache(
    PJ::sdk::descriptor_import::RequestArtifactCache& cache,
    const PJ::sdk::descriptor_import::CleanupPolicy& policy) noexcept {
  try {
    return cache.cleanup(policy);
  } catch (...) {
    PJ::sdk::descriptor_import::CleanupResult failed;
    failed.target_met = false;
    failed.had_errors = true;
    return failed;
  }
}

// ---------------------------------------------------------------------------
// ArtifactWriter
// ---------------------------------------------------------------------------

struct ArtifactWriter::Impl {
  mcap::McapWriter writer;
  bool open = false;
};

ArtifactWriter::ArtifactWriter() : impl_(std::make_unique<Impl>()) {}

ArtifactWriter::~ArtifactWriter() {
  if (impl_ && impl_->open) {
    impl_->writer.terminate();  // no footer: an abandoned partial must not validate
  }
}

bool ArtifactWriter::open(const fs::path& path, const std::string& canonical_descriptor_json, std::string* error) {
  mcap::McapWriterOptions options("");  // profile: none (payloads are Arrow IPC)
  options.library = "toolbox_mosaico cache";
  const auto status = impl_->writer.open(path.string(), options);
  if (!status.ok()) {
    if (error) {
      *error = "artifact open failed: " + status.message;
    }
    return false;
  }
  impl_->open = true;
  // Provenance first, before any message — the canonical bytes are the
  // identity's digest input, embedded VERBATIM.
  mcap::Metadata provenance;
  provenance.name = kProvenanceMetadataName;
  provenance.metadata = {{"json", canonical_descriptor_json}};
  const auto meta_status = impl_->writer.write(provenance);
  if (!meta_status.ok()) {
    if (error) {
      *error = "artifact provenance write failed: " + meta_status.message;
    }
    abort();
    return false;
  }
  return true;
}

std::optional<std::uint16_t> ArtifactWriter::addTopic(
    const ArtifactTopic& topic, const arrow::Schema& schema, std::string* error) {
  if (!impl_->open) {
    if (error) {
      *error = "artifact writer is not open";
    }
    return std::nullopt;
  }
  auto serialized = arrow::ipc::SerializeSchema(schema);
  if (!serialized.ok()) {
    if (error) {
      *error = "schema serialization failed: " + serialized.status().ToString();
    }
    return std::nullopt;
  }
  mcap::Schema mcap_schema(
      topic.name, kSchemaEncoding,
      std::string_view(
          reinterpret_cast<const char*>((*serialized)->data()), static_cast<std::size_t>((*serialized)->size())));
  impl_->writer.addSchema(mcap_schema);
  mcap::Channel channel(topic.name, kMessageEncoding, mcap_schema.id, channelMetadataFor(topic));
  impl_->writer.addChannel(channel);
  return channel.id;
}

std::optional<std::uint16_t> ArtifactWriter::addObjectTopic(const ArtifactTopic& topic, std::string* error) {
  if (!impl_->open) {
    if (error) {
      *error = "artifact writer is not open";
    }
    return std::nullopt;
  }
  mcap::Channel channel(topic.name, kObjectMessageEncoding, /*schemaId=*/0, channelMetadataFor(topic));
  impl_->writer.addChannel(channel);
  return channel.id;
}

bool ArtifactWriter::writeObjectSample(
    std::uint16_t channel_id, std::int64_t log_time_ns, const std::uint8_t* data, std::size_t size,
    std::string* error) {
  if (!impl_->open) {
    if (error) {
      *error = "artifact writer is not open";
    }
    return false;
  }
  if (!writeMcapMessage(
          impl_->writer, channel_id, log_time_ns, reinterpret_cast<const std::byte*>(data),
          static_cast<std::uint64_t>(size), "artifact object write", error)) {
    return false;
  }
  return true;
}

bool ArtifactWriter::writeBatch(
    std::uint16_t channel_id, const arrow::RecordBatch& batch, std::int64_t log_time_ns, std::string* error) {
  if (!impl_->open) {
    if (error) {
      *error = "artifact writer is not open";
    }
    return false;
  }
  auto serialized = arrow::ipc::SerializeRecordBatch(batch, arrow::ipc::IpcWriteOptions::Defaults());
  if (!serialized.ok()) {
    if (error) {
      *error = "record batch serialization failed: " + serialized.status().ToString();
    }
    return false;
  }
  if (!writeMcapMessage(
          impl_->writer, channel_id, log_time_ns, reinterpret_cast<const std::byte*>((*serialized)->data()),
          static_cast<std::uint64_t>((*serialized)->size()), "artifact message write", error)) {
    return false;
  }
  return true;
}

bool ArtifactWriter::close(std::string* error) {
  if (!impl_->open) {
    if (error) {
      *error = "artifact writer is not open";
    }
    return false;
  }
  impl_->writer.close();
  impl_->open = false;
  return true;
}

void ArtifactWriter::abort() {
  if (impl_->open) {
    impl_->writer.terminate();
    impl_->open = false;
  }
}

// ---------------------------------------------------------------------------
// validateArtifact
// ---------------------------------------------------------------------------

bool validateArtifact(const fs::path& file, const std::string& hex, std::string* error) {
  std::string reason;
  if (!summarySpanWithinBudget(file, &reason)) {
    if (error) {
      *error = reason;
    }
    return false;
  }
  mcap::McapReader reader;
  auto status = reader.open(file.string());
  if (!status.ok()) {
    if (error) {
      *error = "not a readable artifact: " + status.message;
    }
    return false;
  }
  status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
  if (!status.ok()) {
    reader.close();
    if (error) {
      *error = "summary unreadable: " + status.message;
    }
    return false;
  }
  if (!reader.statistics().has_value()) {
    reader.close();
    if (error) {
      *error = "missing Statistics record";
    }
    return false;
  }
  mcap::KeyValueMap provenance;
  if (!readBoundedMetadata(reader, file, kProvenanceMetadataName, &provenance, &reason)) {
    reader.close();
    if (error) {
      *error = reason;
    }
    return false;
  }
  const auto json_it = provenance.find("json");
  if (json_it == provenance.end()) {
    reader.close();
    if (error) {
      *error = "embedded descriptor record has no json entry";
    }
    return false;
  }
  // The identity is DEFINED as sha256/128 over the embedded canonical bytes —
  // re-hash them; never trust a stored identity string.
  if (PJ::sdk::descriptor_import::sha256Hex(json_it->second, 32) != hex) {
    reader.close();
    if (error) {
      *error = "embedded descriptor identity mismatch";
    }
    return false;
  }
  reader.close();
  return true;
}

// ---------------------------------------------------------------------------
// readArtifact
// ---------------------------------------------------------------------------

bool readArtifact(
    const fs::path& file, std::vector<ArtifactTopicData>* out_topics, std::string* out_canonical_descriptor_json,
    std::string* error) {
  mcap::McapReader reader;
  auto status = reader.open(file.string());
  if (!status.ok()) {
    if (error) {
      *error = "artifact open failed: " + status.message;
    }
    return false;
  }
  status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
  if (!status.ok()) {
    reader.close();
    if (error) {
      *error = "artifact summary unreadable: " + status.message;
    }
    return false;
  }
  if (out_canonical_descriptor_json) {
    mcap::KeyValueMap provenance;
    std::string reason;
    if (!readBoundedMetadata(reader, file, kProvenanceMetadataName, &provenance, &reason)) {
      reader.close();
      if (error) {
        *error = reason;
      }
      return false;
    }
    const auto json_it = provenance.find("json");
    if (json_it == provenance.end()) {
      reader.close();
      if (error) {
        *error = "embedded descriptor record has no json entry";
      }
      return false;
    }
    *out_canonical_descriptor_json = json_it->second;
  }

  // Channel-id-ordered replay state: schema + memo per channel.
  struct ChannelState {
    ArtifactTopicData data;
    arrow::ipc::DictionaryMemo memo;
    bool schema_ok = false;
  };
  // McapReader::channels()/schemas() return BY VALUE — snapshot them once.
  // Calling reader.schemas().find(...) inline would hand back an iterator
  // into a destroyed temporary (a use-after-free that decoded whatever the
  // allocator recycled — caught as a wrong-schema decode in Release and a
  // null-ish deref at -O0).
  const auto all_channels = reader.channels();
  const auto all_schemas = reader.schemas();
  std::map<std::uint16_t, ChannelState> channels;
  for (const auto& [channel_id, channel] : all_channels) {
    ChannelState& state = channels[channel_id];
    state.data.info.name = channel->topic;
    const auto meta = [&](const char* key) {
      const auto it = channel->metadata.find(key);
      return it == channel->metadata.end() ? std::string() : it->second;
    };
    state.data.info.ontology_tag = meta(kMetaOntologyTag);
    state.data.info.canonical_metadata = meta(kMetaCanonicalMetadata);
    state.data.info.timestamp_column = meta(kMetaTimestampColumn);
    if (channel->messageEncoding == kObjectMessageEncoding) {
      // Object channel: canonical blobs, no Arrow schema to decode.
      state.data.is_object = true;
      state.schema_ok = true;
      continue;
    }
    const auto schema_it = all_schemas.find(channel->schemaId);
    if (schema_it == all_schemas.end()) {
      reader.close();
      if (error) {
        *error = "channel \"" + channel->topic + "\" has no schema record";
      }
      return false;
    }
    // Same owned-aligned-copy rule as the batch payloads below.
    auto schema_buffer = arrow::AllocateBuffer(static_cast<std::int64_t>(schema_it->second->data.size()));
    if (!schema_buffer.ok()) {
      reader.close();
      if (error) {
        *error = "schema buffer allocation failed: " + schema_buffer.status().ToString();
      }
      return false;
    }
    std::memcpy((*schema_buffer)->mutable_data(), schema_it->second->data.data(), schema_it->second->data.size());
    arrow::io::BufferReader buffer(std::shared_ptr<arrow::Buffer>(std::move(*schema_buffer)));
    auto schema = arrow::ipc::ReadSchema(&buffer, &state.memo);
    if (!schema.ok()) {
      reader.close();
      if (error) {
        *error = "channel \"" + channel->topic + "\" schema undecodable: " + schema.status().ToString();
      }
      return false;
    }
    state.data.schema = *schema;
    state.schema_ok = true;
  }

  bool failed = false;
  std::string fail_reason;
  auto on_problem = [&](const mcap::Status& problem) {
    failed = true;
    fail_reason = problem.message;
  };
  for (const auto& view : reader.readMessages(on_problem)) {
    const std::uint16_t channel_id = view.message.channelId;
    auto it = channels.find(channel_id);
    if (it == channels.end() || !it->second.schema_ok) {
      reader.close();
      if (error) {
        *error = "message on unknown channel " + std::to_string(channel_id);
      }
      return false;
    }
    if (it->second.data.is_object) {
      // Object channel: the payload IS the canonical blob; copy it verbatim.
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(view.message.data);
      it->second.data.object_samples.push_back(
          {static_cast<std::int64_t>(view.message.logTime),
           std::vector<std::uint8_t>(bytes, bytes + view.message.dataSize)});
      continue;
    }
    // Copy the payload into an OWNED, arrow-allocated (8-byte-aligned) buffer:
    // the message view's memory is transient (reused as iteration advances) and
    // arbitrarily aligned inside the chunk, while the decoded batch zero-copies
    // its input — so it must own aligned bytes.
    auto owned = arrow::AllocateBuffer(static_cast<std::int64_t>(view.message.dataSize));
    if (!owned.ok()) {
      reader.close();
      if (error) {
        *error = "payload buffer allocation failed: " + owned.status().ToString();
      }
      return false;
    }
    std::memcpy((*owned)->mutable_data(), view.message.data, view.message.dataSize);
    arrow::io::BufferReader payload(std::shared_ptr<arrow::Buffer>(std::move(*owned)));
    auto batch = arrow::ipc::ReadRecordBatch(
        it->second.data.schema, &it->second.memo, arrow::ipc::IpcReadOptions::Defaults(), &payload);
    if (!batch.ok()) {
      reader.close();
      if (error) {
        *error = "batch undecodable on \"" + it->second.data.info.name + "\": " + batch.status().ToString();
      }
      return false;
    }
    it->second.data.batches.push_back(*batch);
    it->second.data.batch_log_times_ns.push_back(static_cast<std::int64_t>(view.message.logTime));
  }
  reader.close();
  if (failed) {
    if (error) {
      *error = "artifact message scan failed: " + fail_reason;
    }
    return false;
  }
  if (out_topics) {
    out_topics->clear();
    out_topics->reserve(channels.size());
    for (auto& [channel_id, state] : channels) {
      (void)channel_id;
      out_topics->push_back(std::move(state.data));
    }
  }
  return true;
}

}  // namespace mosaico
