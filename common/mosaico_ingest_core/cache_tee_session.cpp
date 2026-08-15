// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "cache_tee_session.hpp"

#include <arrow/api.h>

namespace mosaico {

namespace {

// The batch's representative log time for the artifact message: the first
// row's value of `ts_field` when it is an int64 column, else 0 (the loader
// replays scalars from the timestamp COLUMN, so this only orders messages
// inside the container).
std::int64_t batchLogTime(const arrow::RecordBatch& batch, const std::string& ts_field) {
  if (ts_field.empty() || batch.num_rows() == 0) {
    return 0;
  }
  const auto column = batch.GetColumnByName(ts_field);
  if (!column || column->type_id() != arrow::Type::INT64) {
    return 0;
  }
  const auto& typed = static_cast<const arrow::Int64Array&>(*column);
  return typed.IsNull(0) ? 0 : typed.Value(0);
}

}  // namespace

CacheTeeSession::CacheTeeSession(const SourceDescriptor& descriptor, const std::filesystem::path& cache_root_override)
    : cache_(SessionFileCache::at(cache_root_override, validateArtifact, nullptr)) {
  // Self-check: only a descriptor that round-trips our own validation may
  // name an artifact (e.g. a scheme-less URI typed into the connect box
  // disarms the tee instead of producing an unloadable record).
  std::string error;
  const std::string json = toSourceDescriptorJson(descriptor);
  if (!parseSourceDescriptor(json, &error).has_value()) {
    disarm("descriptor not cacheable: " + error);
    return;
  }
  identity_ = descriptorIdentity(descriptor);
  descriptor_json_ = json;
  lock_ = cache_.tryLockForMaterialize(identity_, &error);
  if (!lock_.has_value()) {
    // Contended = another materialization or a live read lease (the
    // refusal-while-referenced rule); either way: eager-only.
    disarm("cache unavailable: " + error);
    return;
  }
  if (!writer_.open(cache_.partialPathFor(*lock_), canonicalSourceDescriptorJson(descriptor), &error)) {
    disarm("artifact writer: " + error);
    lock_.reset();
    return;
  }
  armed_ = true;
}

CacheTeeSession::~CacheTeeSession() {
  if (!finished_) {
    (void)finish(/*complete=*/false);  // abandoned session: abort + delete partial
  }
}

bool CacheTeeSession::armed() const {
  return armed_;
}

const std::string& CacheTeeSession::disarmReason() const {
  return disarm_reason_;
}

const std::string& CacheTeeSession::identity() const {
  return identity_;
}

const std::string& CacheTeeSession::descriptorJson() const {
  return descriptor_json_;
}

void CacheTeeSession::disarm(std::string reason) {
  if (armed_ || disarm_reason_.empty()) {
    disarm_reason_ = std::move(reason);
  }
  if (armed_) {
    writer_.abort();
    armed_ = false;
  }
}

void CacheTeeSession::teeScalarTopic(
    const std::string& topic, const arrow::Schema& schema, const std::string& ts_field,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
  if (!armed_) {
    return;
  }
  std::string error;
  const auto channel = writer_.addTopic(
      ArtifactTopic{.name = topic, .ontology_tag = "", .canonical_metadata = "", .timestamp_column = ts_field}, schema,
      &error);
  if (!channel.has_value()) {
    disarm("scalar tee (" + topic + "): " + error);
    return;
  }
  for (const auto& batch : batches) {
    if (!batch) {
      continue;
    }
    if (!writer_.writeBatch(*channel, *batch, batchLogTime(*batch, ts_field), &error)) {
      disarm("scalar tee (" + topic + "): " + error);
      return;
    }
  }
}

std::function<void(std::int64_t, const std::uint8_t*, std::size_t)> CacheTeeSession::objectTee(
    const std::string& topic, const std::string& ontology_tag, const std::string& canonical_metadata) {
  return
      [this, topic, ontology_tag, canonical_metadata](std::int64_t ts_ns, const std::uint8_t* data, std::size_t size) {
        if (!armed_) {
          return;
        }
        std::string error;
        auto it = object_channels_.find(topic);
        if (it == object_channels_.end()) {
          const auto channel = writer_.addObjectTopic(
              ArtifactTopic{
                  .name = topic,
                  .ontology_tag = ontology_tag,
                  .canonical_metadata = canonical_metadata,
                  .timestamp_column = ""},
              &error);
          if (!channel.has_value()) {
            disarm("object tee (" + topic + "): " + error);
            return;
          }
          it = object_channels_.emplace(topic, *channel).first;
        }
        if (!writer_.writeObjectSample(it->second, ts_ns, data, size, &error)) {
          disarm("object tee (" + topic + "): " + error);
        }
      };
}

std::optional<CacheTeeSession::Finalized> CacheTeeSession::finish(bool complete) {
  finished_ = true;
  if (!armed_ || !complete) {
    if (armed_) {
      disarm(complete ? "finish failed" : "fetch incomplete");
    }
    // Delete an orphaned partial (the writer wrote no valid footer, so
    // finalize would reject it anyway — remove it eagerly).
    if (lock_.has_value()) {
      std::error_code ec;
      std::filesystem::remove(cache_.partialPathFor(*lock_), ec);
      lock_.reset();
    }
    return std::nullopt;
  }
  std::string error;
  SessionFileCache::ExpectedContent expected;
  if (!writer_.close(&expected, &error)) {
    disarm("artifact close: " + error);
    std::error_code ec;
    std::filesystem::remove(cache_.partialPathFor(*lock_), ec);
    lock_.reset();
    return std::nullopt;
  }
  if (!cache_.finalize(*lock_, expected, &error)) {
    disarm("cache finalize: " + error);  // finalize already removed the partial
    lock_.reset();
    return std::nullopt;
  }
  Finalized finalized;
  finalized.path = cache_.pathFor(identity_);
  finalized.lease = SessionFileCache::toSharedLease(std::move(*lock_), &error);
  lock_.reset();
  armed_ = false;
  return finalized;
}

}  // namespace mosaico
