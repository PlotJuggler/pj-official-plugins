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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mcap/reader.hpp>
#include <mcap/writer.hpp>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <set>
#include <stdexcept>

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
#if defined(_WIN32)
  if (auto value = PJ::sdk::getEnv("LOCALAPPDATA")) {
    return fs::path(*value) / "mosaico" / "sessions";
  }
  if (auto value = PJ::sdk::getEnv("USERPROFILE")) {
    return fs::path(*value) / "AppData" / "Local" / "mosaico" / "sessions";
  }
  if (error != nullptr) {
    *error = "cache root unresolvable (MOSAICO_CACHE_DIR, LOCALAPPDATA and USERPROFILE all unset)";
  }
#else
  if (auto value = PJ::sdk::getEnv("XDG_CACHE_HOME")) {
    return fs::path(*value) / "mosaico" / "sessions";
  }
  if (auto value = PJ::sdk::getEnv("HOME")) {
    return fs::path(*value) / ".cache" / "mosaico" / "sessions";
  }
  if (error != nullptr) {
    *error = "cache root unresolvable (MOSAICO_CACHE_DIR, XDG_CACHE_HOME and HOME all unset)";
  }
#endif
  return {};
}

PJ::sdk::descriptor_import::RequestArtifactCache makeArtifactCache(
    const fs::path& configured_root, std::string* error) {
  fs::path root = configured_root.empty() ? standardCacheRoot(error) : configured_root;
  if (!root.empty() && root.is_relative()) {
    // The ABI requires absolute paths, and a relative root would change
    // meaning with the process cwd.
    std::error_code ec;
    const fs::path absolute = fs::absolute(root, ec);
    if (!ec) {
      root = absolute;
    }
  }
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

std::string utf8Path(const fs::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

bool quarantineArtifact(const fs::path& artifact, std::string* error) {
  fs::path quarantined = artifact;
  quarantined += ".corrupt";
  std::error_code ec;
  fs::remove(quarantined, ec);  // an older quarantine of the same identity
  ec.clear();
  fs::rename(artifact, quarantined, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "could not quarantine " + utf8Path(artifact) + ": " + ec.message();
    }
    return false;
  }
  return true;
}

namespace {

// The vendored MCAP writer takes a narrow path; the conversion can throw on
// Windows for a path outside the execution code page — report, never crash.
std::optional<std::string> narrowPath(const fs::path& path, std::string* error) {
  try {
    return path.string();
  } catch (const std::exception& e) {
    if (error != nullptr) {
      *error = "cache path is not representable in the system code page: " + std::string(e.what());
    }
    return std::nullopt;
  }
}

}  // namespace

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
  const auto narrow = narrowPath(path, error);
  if (!narrow.has_value()) {
    return false;
  }
  const auto status = impl_->writer.open(*narrow, options);
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
  std::ifstream input(file, std::ios::binary);  // native path: no narrowing
  if (!input) {
    if (error) {
      *error = "cannot open artifact";
    }
    return false;
  }
  mcap::FileStreamReader source(input);
  mcap::McapReader reader;
  auto status = reader.open(source);
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
  // Bounded content check from the summary: every channel must be a
  // descriptor topic, and an artifact with messages must have channels. A
  // corrupted or foreign body that kept a valid footer fails here instead of
  // being a sticky hit; body corruption past this point is caught by replay,
  // which quarantines the file.
  {
    std::set<std::string> topics;
    const auto canonical = nlohmann::json::parse(json_it->second, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (canonical.is_object() && canonical.contains("topics") && canonical["topics"].is_array()) {
      for (const auto& topic : canonical["topics"]) {
        if (topic.is_string()) {
          topics.insert(topic.get<std::string>());
        }
      }
    }
    const auto channels = reader.channels();  // returned BY VALUE: snapshot once
    for (const auto& entry : channels) {
      const auto& channel = entry.second;
      if (channel == nullptr || topics.count(channel->topic) == 0) {
        reader.close();
        if (error) {
          *error = "artifact channel is not a descriptor topic: " + (channel ? channel->topic : std::string("<null>"));
        }
        return false;
      }
    }
    if (channels.empty() && reader.statistics()->messageCount > 0) {
      reader.close();
      if (error) {
        *error = "artifact has messages but no channels";
      }
      return false;
    }
  }
  reader.close();
  return true;
}

// ---------------------------------------------------------------------------
// readArtifact
// ---------------------------------------------------------------------------

namespace {

// Intentionally matches mcap::ParallelReadOptions' native default
// maxChunkUncompressedSize policy without instantiating the parallel engine in
// this MCAP_IMPLEMENTATION translation unit.
constexpr std::uint64_t kReplayDecodedChunkBudget = 2ull << 30;

std::uint64_t parseReplayLe64(const unsigned char* bytes) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  }
  return value;
}

// McapReader::open() and readSummary() allocate from record body lengths. Raw
// framing checks must therefore precede both parser entry points. The cache
// read lease keeps the artifact immutable while this separate raw stream and
// the MCAP stream are used synchronously.
bool preflightReplayRecords(std::ifstream& raw, std::uintmax_t file_size, std::string* error) {
  const auto fail = [&](std::string reason) {
    if (error) {
      *error = "artifact replay framing preflight failed: " + std::move(reason);
    }
    return false;
  };
  const auto read_at = [&](std::uintmax_t offset, unsigned char* bytes, std::size_t size) {
    if (offset > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
      return false;
    }
    raw.clear();
    raw.seekg(static_cast<std::streamoff>(offset));
    raw.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    return static_cast<bool>(raw);
  };

  if (file_size < kMcapTailBytes + sizeof(mcap::Magic) + 9) {
    return fail("artifact is too small");
  }
  unsigned char footer[kMcapTailBytes] = {};
  const std::uintmax_t footer_offset = file_size - kMcapTailBytes;
  if (!read_at(footer_offset, footer, sizeof(footer)) ||
      std::memcmp(footer + kMcapTailBytes - sizeof(mcap::Magic), mcap::Magic, sizeof(mcap::Magic)) != 0 ||
      footer[0] != static_cast<unsigned char>(mcap::OpCode::Footer) || parseReplayLe64(footer + 1) != 20) {
    return fail("footer framing is invalid");
  }
  const std::uint64_t summary_start = parseReplayLe64(footer + 9);
  if (summary_start > footer_offset || (summary_start != 0 && footer_offset - summary_start > kQuerySummaryBudget)) {
    return fail("summary range is outside the bounded query budget");
  }

  unsigned char leading[sizeof(mcap::Magic) + 9] = {};
  if (!read_at(0, leading, sizeof(leading)) || std::memcmp(leading, mcap::Magic, sizeof(mcap::Magic)) != 0 ||
      leading[sizeof(mcap::Magic)] != static_cast<unsigned char>(mcap::OpCode::Header)) {
    return fail("leading magic/Header record is invalid");
  }
  const std::uint64_t header_size = parseReplayLe64(leading + sizeof(mcap::Magic) + 1);
  const std::uintmax_t section_limit = summary_start == 0 ? footer_offset : summary_start;
  const std::uintmax_t header_offset = sizeof(mcap::Magic);
  if (header_size > kQuerySummaryBudget || section_limit < header_offset + 9 ||
      header_size > section_limit - header_offset - 9) {
    return fail("Header body length is outside the bounded data section");
  }

  if (summary_start == 0) {
    return true;
  }
  std::uintmax_t offset = summary_start;
  while (offset < footer_offset) {
    if (footer_offset - offset < 9) {
      return fail("truncated summary record header");
    }
    unsigned char record_header[9] = {};
    if (!read_at(offset, record_header, sizeof(record_header))) {
      return fail("summary record header is unreadable");
    }
    if (record_header[0] == static_cast<unsigned char>(mcap::OpCode::Chunk)) {
      return fail("Chunk record is forbidden in the summary section");
    }
    const std::uint64_t body_size = parseReplayLe64(record_header + 1);
    const std::uintmax_t remaining = footer_offset - offset - 9;
    if (body_size > remaining) {
      return fail("summary record body lies outside the summary section");
    }
    offset += 9 + body_size;
  }
  return true;
}

}  // namespace

bool preflightArtifactForReplay(const fs::path& file, std::string* error) {
  std::string reason;
  if (!summarySpanWithinBudget(file, &reason)) {
    if (error) {
      *error = std::move(reason);
    }
    return false;
  }
  std::error_code size_error;
  const std::uintmax_t file_size = fs::file_size(file, size_error);
  std::ifstream raw(file, std::ios::binary);
  if (size_error || !raw) {
    if (error) {
      *error = "artifact raw preflight open failed";
    }
    return false;
  }
  return preflightReplayRecords(raw, file_size, error);
}

struct ArtifactStreamReader::Impl {
  struct ChannelState {
    std::uint16_t channel_id = 0;
    std::unique_ptr<arrow::ipc::DictionaryMemo> memo = std::make_unique<arrow::ipc::DictionaryMemo>();
  };

  mcap::McapReader reader;
  std::uintmax_t file_size = 0;
  std::ifstream raw_file;
  std::vector<ChannelState> channels;
  std::vector<ArtifactTopicSummary> summaries;
  std::vector<mcap::ChunkIndex> chunk_indexes;
  std::uint64_t message_count = 0;
  bool open = false;

  std::size_t active_topic = 0;
  std::uint64_t active_messages_read = 0;
  bool active = false;
  bool needs_advance = false;
  std::vector<const mcap::ChunkIndex*> active_chunks;
  std::size_t next_chunk = 0;
  std::unique_ptr<mcap::TypedRecordReader> chunk_reader;
  mcap::Message current_message;
  bool have_message = false;

  static std::uint32_t parseLe32(const unsigned char* bytes) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    }
    return value;
  }

  bool preflightChunk(const mcap::ChunkIndex& chunk, std::string* error) {
    const auto fail = [&](std::string reason) {
      if (error) {
        *error = "artifact chunk preflight failed: " + std::move(reason);
      }
      return false;
    };
    constexpr std::uint64_t budget = kReplayDecodedChunkBudget;
    if (chunk.compressedSize > budget) {
      return fail(
          "compressedSize " + std::to_string(chunk.compressedSize) + " exceeds the native " + std::to_string(budget) +
          "-byte replay cap");
    }
    if (chunk.uncompressedSize > budget) {
      return fail(
          "uncompressedSize " + std::to_string(chunk.uncompressedSize) + " exceeds the native " +
          std::to_string(budget) + "-byte replay cap");
    }
    if (chunk.chunkLength < 9 || chunk.chunkStartOffset > file_size ||
        chunk.chunkLength > file_size - chunk.chunkStartOffset) {
      return fail("summary ChunkIndex range lies outside the artifact body");
    }

    // Raw-read the record framing and fixed Chunk fields before RecordReader
    // sees them. RecordReader trusts the body length and may resize its buffer;
    // these checks prove that length is the bounded indexed chunk we approved.
    unsigned char record_header[9] = {};
    raw_file.clear();
    raw_file.seekg(static_cast<std::streamoff>(chunk.chunkStartOffset));
    raw_file.read(reinterpret_cast<char*>(record_header), sizeof(record_header));
    if (!raw_file || record_header[0] != static_cast<unsigned char>(mcap::OpCode::Chunk)) {
      return fail("raw body has no Chunk record at the indexed offset");
    }
    const std::uint64_t body_size = parseReplayLe64(record_header + 1);
    if (body_size > std::numeric_limits<std::uint64_t>::max() - 9 || body_size + 9 != chunk.chunkLength) {
      return fail("raw Chunk record size disagrees with ChunkIndex.chunkLength");
    }

    // message_start/end, uncompressed_size, CRC, compression string length.
    unsigned char fixed_body[32] = {};
    raw_file.read(reinterpret_cast<char*>(fixed_body), sizeof(fixed_body));
    if (!raw_file) {
      return fail("raw Chunk fixed header is truncated");
    }
    const std::uint64_t raw_uncompressed = parseReplayLe64(fixed_body + 16);
    const std::uint32_t compression_size = parseLe32(fixed_body + 28);
    if (raw_uncompressed > budget) {
      return fail("raw Chunk uncompressedSize exceeds the native replay cap");
    }
    if (raw_uncompressed != chunk.uncompressedSize) {
      return fail("raw Chunk uncompressedSize disagrees with the bounded ChunkIndex");
    }
    if (compression_size != chunk.compression.size() || static_cast<std::uint64_t>(compression_size) + 40 > body_size) {
      return fail("raw Chunk compression field disagrees with the ChunkIndex");
    }
    std::string compression(compression_size, '\0');
    raw_file.read(compression.data(), static_cast<std::streamsize>(compression.size()));
    unsigned char compressed_size_bytes[8] = {};
    raw_file.read(reinterpret_cast<char*>(compressed_size_bytes), sizeof(compressed_size_bytes));
    if (!raw_file || compression != chunk.compression) {
      return fail("raw Chunk compression value disagrees with the ChunkIndex");
    }
    const std::uint64_t raw_compressed = parseReplayLe64(compressed_size_bytes);
    if (raw_compressed > budget) {
      return fail("raw Chunk compressedSize exceeds the native replay cap");
    }
    if (raw_compressed != chunk.compressedSize) {
      return fail("raw Chunk compressedSize disagrees with the bounded ChunkIndex");
    }
    const std::uint64_t framing = 40 + static_cast<std::uint64_t>(compression_size);
    if (raw_compressed > std::numeric_limits<std::uint64_t>::max() - framing || framing + raw_compressed != body_size) {
      return fail("raw Chunk body size does not match its compressed payload");
    }
    return true;
  }

  void resetPass() {
    chunk_reader.reset();
    active_chunks.clear();
    next_chunk = 0;
    have_message = false;
    active = false;
    needs_advance = false;
    active_messages_read = 0;
  }

  bool prepareMessage(const mcap::Message** out, std::string* error) {
    *out = nullptr;
    if (!active) {
      if (error) {
        *error = "no artifact topic selected";
      }
      return false;
    }
    if (summaries[active_topic].message_count == 0) {
      return true;
    }
    if (needs_advance) {
      needs_advance = false;
      have_message = false;
    }

    while (!have_message) {
      if (!chunk_reader) {
        if (next_chunk == active_chunks.size()) {
          const auto expected = summaries[active_topic].message_count;
          if (active_messages_read != expected) {
            if (error) {
              *error = "channel \"" + summaries[active_topic].info.name + "\" message count mismatch (summary " +
                       std::to_string(expected) + ", read " + std::to_string(active_messages_read) + ")";
            }
            return false;
          }
          return true;
        }
        const mcap::ChunkIndex& chunk = *active_chunks[next_chunk++];
        if (!preflightChunk(chunk, error)) {
          return false;
        }
        try {
          chunk_reader = std::make_unique<mcap::TypedRecordReader>(
              *reader.dataSource(), chunk.chunkStartOffset, chunk.chunkStartOffset + chunk.chunkLength);
        } catch (const std::bad_alloc&) {
          if (error) {
            *error = "artifact chunk reader allocation failed within the replay cap";
          }
          return false;
        }
        chunk_reader->onMessage = [this](
                                      const mcap::Message& message, mcap::ByteOffset, std::optional<mcap::ByteOffset>) {
          if (message.channelId == channels[active_topic].channel_id) {
            current_message = message;
            have_message = true;
          }
        };
      }
      bool found_record = false;
      try {
        found_record = chunk_reader->next();
      } catch (const std::bad_alloc&) {
        if (error) {
          *error = "artifact chunk decompression allocation failed within the replay cap";
        }
        return false;
      } catch (const std::length_error& ex) {
        if (error) {
          *error = "artifact chunk decompression size failed: " + std::string(ex.what());
        }
        return false;
      }
      const auto& status = chunk_reader->status();
      if (!status.ok()) {
        if (error) {
          *error = "artifact message scan failed: " + status.message;
        }
        return false;
      }
      if (!found_record) {
        chunk_reader.reset();
      }
    }
    *out = &current_message;
    return true;
  }
};

ArtifactStreamReader::ArtifactStreamReader() : impl_(std::make_unique<Impl>()) {}

ArtifactStreamReader::~ArtifactStreamReader() {
  close();
}

bool ArtifactStreamReader::open(const fs::path& file, std::string* out_canonical_descriptor_json, std::string* error) {
  close();
  if (!preflightArtifactForReplay(file, error)) {
    return false;
  }
  std::error_code size_error;
  impl_->file_size = fs::file_size(file, size_error);
  impl_->raw_file.open(file, std::ios::binary);
  if (size_error || !impl_->raw_file) {
    if (error) {
      *error = "artifact raw preflight open failed";
    }
    close();
    return false;
  }
  auto status = impl_->reader.open(file.string());
  if (!status.ok()) {
    if (error) {
      *error = "artifact open failed: " + status.message;
    }
    close();
    return false;
  }
  impl_->open = true;
  status = impl_->reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
  if (!status.ok()) {
    if (error) {
      *error = "artifact summary unreadable: " + status.message;
    }
    close();
    return false;
  }
  if (!impl_->reader.statistics().has_value()) {
    if (error) {
      *error = "missing Statistics record";
    }
    close();
    return false;
  }
  if (out_canonical_descriptor_json) {
    mcap::KeyValueMap provenance;
    std::string reason;
    if (!readBoundedMetadata(impl_->reader, file, kProvenanceMetadataName, &provenance, &reason)) {
      if (error) {
        *error = reason;
      }
      close();
      return false;
    }
    const auto json_it = provenance.find("json");
    if (json_it == provenance.end()) {
      if (error) {
        *error = "embedded descriptor record has no json entry";
      }
      close();
      return false;
    }
    *out_canonical_descriptor_json = json_it->second;
  }

  // McapReader accessors return maps by value. Snapshot them so schema/channel
  // pointers and iterators stay valid throughout summary decoding.
  const auto all_channels = impl_->reader.channels();
  const auto all_schemas = impl_->reader.schemas();
  impl_->chunk_indexes = impl_->reader.chunkIndexes();
  std::sort(
      impl_->chunk_indexes.begin(), impl_->chunk_indexes.end(),
      [](const mcap::ChunkIndex& lhs, const mcap::ChunkIndex& rhs) {
        return lhs.chunkStartOffset < rhs.chunkStartOffset;
      });
  for (const auto& chunk : impl_->chunk_indexes) {
    constexpr std::uint64_t budget = kReplayDecodedChunkBudget;
    if (chunk.compressedSize > budget || chunk.uncompressedSize > budget) {
      if (error) {
        *error = "artifact ChunkIndex exceeds the native " + std::to_string(budget) + "-byte replay cap";
      }
      close();
      return false;
    }
    if (chunk.chunkLength < 9 || chunk.chunkStartOffset > impl_->file_size ||
        chunk.chunkLength > impl_->file_size - chunk.chunkStartOffset) {
      if (error) {
        *error = "artifact ChunkIndex range lies outside the artifact body";
      }
      close();
      return false;
    }
  }
  const auto& counts = impl_->reader.statistics()->channelMessageCounts;
  std::map<std::string, std::uint16_t> topic_names;
  std::map<std::uint16_t, mcap::ChannelPtr> ordered_channels(all_channels.begin(), all_channels.end());
  impl_->channels.reserve(ordered_channels.size());
  impl_->summaries.reserve(ordered_channels.size());
  for (const auto& [channel_id, channel] : ordered_channels) {
    const auto [name_it, inserted] = topic_names.emplace(channel->topic, channel_id);
    if (!inserted) {
      if (error) {
        *error = "duplicate artifact topic \"" + channel->topic + "\" on channels " + std::to_string(name_it->second) +
                 " and " + std::to_string(channel_id);
      }
      close();
      return false;
    }
    Impl::ChannelState state;
    ArtifactTopicSummary summary;
    state.channel_id = channel_id;
    summary.info.name = channel->topic;
    const auto meta = [&](const char* key) {
      const auto it = channel->metadata.find(key);
      return it == channel->metadata.end() ? std::string() : it->second;
    };
    summary.info.ontology_tag = meta(kMetaOntologyTag);
    summary.info.canonical_metadata = meta(kMetaCanonicalMetadata);
    summary.info.timestamp_column = meta(kMetaTimestampColumn);
    const auto count_it = counts.find(channel_id);
    summary.message_count = count_it == counts.end() ? 0 : count_it->second;
    if (std::numeric_limits<std::uint64_t>::max() - impl_->message_count < summary.message_count) {
      if (error) {
        *error = "artifact message count overflow";
      }
      close();
      return false;
    }
    impl_->message_count += summary.message_count;

    if (channel->messageEncoding == kObjectMessageEncoding) {
      summary.is_object = true;
    } else {
      const auto schema_it = all_schemas.find(channel->schemaId);
      if (schema_it == all_schemas.end()) {
        if (error) {
          *error = "channel \"" + channel->topic + "\" has no schema record";
        }
        close();
        return false;
      }
      if (schema_it->second->data.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        if (error) {
          *error = "channel \"" + channel->topic + "\" schema exceeds Arrow's decoded-size budget";
        }
        close();
        return false;
      }
      auto schema_buffer = arrow::AllocateBuffer(static_cast<std::int64_t>(schema_it->second->data.size()));
      if (!schema_buffer.ok()) {
        if (error) {
          *error = "schema buffer allocation failed: " + schema_buffer.status().ToString();
        }
        close();
        return false;
      }
      std::memcpy((*schema_buffer)->mutable_data(), schema_it->second->data.data(), schema_it->second->data.size());
      arrow::io::BufferReader buffer(std::shared_ptr<arrow::Buffer>(std::move(*schema_buffer)));
      auto schema = arrow::ipc::ReadSchema(&buffer, state.memo.get());
      if (!schema.ok()) {
        if (error) {
          *error = "channel \"" + channel->topic + "\" schema undecodable: " + schema.status().ToString();
        }
        close();
        return false;
      }
      summary.schema = *schema;
    }
    impl_->summaries.push_back(std::move(summary));
    impl_->channels.push_back(std::move(state));
  }
  if (impl_->message_count != impl_->reader.statistics()->messageCount) {
    if (error) {
      *error = "artifact channel message counts do not match the total Statistics count";
    }
    close();
    return false;
  }
  return true;
}

void ArtifactStreamReader::close() {
  if (!impl_) {
    return;
  }
  impl_->resetPass();
  if (impl_->open) {
    impl_->reader.close();
  }
  if (impl_->raw_file.is_open()) {
    impl_->raw_file.close();
  }
  impl_->raw_file.clear();
  impl_->file_size = 0;
  impl_->open = false;
  impl_->channels.clear();
  impl_->summaries.clear();
  impl_->chunk_indexes.clear();
  impl_->message_count = 0;
}

const std::vector<ArtifactTopicSummary>& ArtifactStreamReader::topics() const {
  return impl_->summaries;
}

std::uint64_t ArtifactStreamReader::messageCount() const {
  return impl_->message_count;
}

bool ArtifactStreamReader::startTopic(std::size_t topic_index, std::string* error) {
  impl_->resetPass();
  if (!impl_->open) {
    if (error) {
      *error = "artifact reader is not open";
    }
    return false;
  }
  if (topic_index >= impl_->channels.size()) {
    if (error) {
      *error = "artifact topic index out of range";
    }
    return false;
  }
  impl_->active = true;
  impl_->active_topic = topic_index;
  if (impl_->summaries[topic_index].message_count == 0) {
    return true;
  }

  const auto channel_id = impl_->channels[topic_index].channel_id;
  for (const auto& chunk : impl_->chunk_indexes) {
    if (chunk.messageIndexOffsets.find(channel_id) == chunk.messageIndexOffsets.end()) {
      continue;
    }
    if (chunk.chunkLength > std::numeric_limits<mcap::ByteOffset>::max() - chunk.chunkStartOffset) {
      if (error) {
        *error = "artifact chunk range overflow";
      }
      impl_->resetPass();
      return false;
    }
    impl_->active_chunks.push_back(&chunk);
  }
  if (impl_->active_chunks.empty()) {
    if (error) {
      *error = "channel \"" + impl_->summaries[topic_index].info.name + "\" has messages but no message index";
    }
    impl_->resetPass();
    return false;
  }
  return true;
}

bool ArtifactStreamReader::readNextScalar(
    std::shared_ptr<arrow::RecordBatch>* out_batch, std::int64_t* out_log_time_ns, std::string* error) {
  if (!out_batch) {
    if (error) {
      *error = "scalar output pointer is null";
    }
    return false;
  }
  *out_batch = nullptr;
  if (!impl_->active || impl_->summaries[impl_->active_topic].is_object) {
    if (error) {
      *error = "selected artifact topic is not scalar";
    }
    return false;
  }
  const mcap::Message* view = nullptr;
  if (!impl_->prepareMessage(&view, error)) {
    return false;
  }
  if (!view) {
    return true;
  }
  if (view->dataSize > kReplayDecodedChunkBudget) {
    if (error) {
      *error = "batch payload on \"" + impl_->summaries[impl_->active_topic].info.name + "\" exceeds the native " +
               std::to_string(kReplayDecodedChunkBudget) + "-byte replay cap";
    }
    return false;
  }
  auto owned = arrow::AllocateBuffer(static_cast<std::int64_t>(view->dataSize));
  if (!owned.ok()) {
    if (error) {
      *error = "payload buffer allocation failed: " + owned.status().ToString();
    }
    return false;
  }
  std::memcpy((*owned)->mutable_data(), view->data, view->dataSize);
  arrow::io::BufferReader payload(std::shared_ptr<arrow::Buffer>(std::move(*owned)));
  auto& channel = impl_->channels[impl_->active_topic];
  const auto& topic = impl_->summaries[impl_->active_topic];
  auto batch =
      arrow::ipc::ReadRecordBatch(topic.schema, channel.memo.get(), arrow::ipc::IpcReadOptions::Defaults(), &payload);
  if (!batch.ok()) {
    if (error) {
      *error = "batch undecodable on \"" + topic.info.name + "\": " + batch.status().ToString();
    }
    return false;
  }
  *out_batch = *batch;
  if (out_log_time_ns) {
    *out_log_time_ns = static_cast<std::int64_t>(view->logTime);
  }
  ++impl_->active_messages_read;
  impl_->needs_advance = true;
  return true;
}

bool ArtifactStreamReader::readNextObject(ArtifactObjectSample* out_sample, bool* out_has_sample, std::string* error) {
  if (!out_sample || !out_has_sample) {
    if (error) {
      *error = "object output pointer is null";
    }
    return false;
  }
  *out_has_sample = false;
  if (!impl_->active || !impl_->summaries[impl_->active_topic].is_object) {
    if (error) {
      *error = "selected artifact topic is not an object topic";
    }
    return false;
  }
  const mcap::Message* view = nullptr;
  if (!impl_->prepareMessage(&view, error)) {
    return false;
  }
  if (!view) {
    return true;
  }
  if (view->dataSize > kReplayDecodedChunkBudget) {
    if (error) {
      *error = "object payload on \"" + impl_->summaries[impl_->active_topic].info.name + "\" exceeds the native " +
               std::to_string(kReplayDecodedChunkBudget) + "-byte replay cap";
    }
    return false;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(view->data);
  out_sample->log_time_ns = static_cast<std::int64_t>(view->logTime);
  try {
    out_sample->payload.assign(bytes, bytes + static_cast<std::size_t>(view->dataSize));
  } catch (const std::bad_alloc&) {
    if (error) {
      *error = "object payload allocation failed within the replay cap";
    }
    return false;
  } catch (const std::length_error& ex) {
    if (error) {
      *error = "object payload size failed: " + std::string(ex.what());
    }
    return false;
  }
  *out_has_sample = true;
  ++impl_->active_messages_read;
  impl_->needs_advance = true;
  return true;
}

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
