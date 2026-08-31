// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "descriptor_import/artifact_capture.hpp"

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

ArtifactCapture::ArtifactCapture(const SourceDescriptor& descriptor, const std::filesystem::path& cache_root_override)
    : cache_(makeArtifactCache(cache_root_override)) {
  // Self-check: only a descriptor that round-trips our own validation may
  // name an artifact (e.g. a scheme-less URI typed into the connect box
  // disarms capture instead of producing an unloadable record).
  std::string error;
  const std::string json = toSourceDescriptorJson(descriptor);
  if (!parseSourceDescriptor(json, &error).has_value()) {
    disarm("descriptor not cacheable: " + error);
    return;
  }
  identity_ = descriptorIdentity(descriptor);
  descriptor_json_ = json;
  auto transaction = cache_.beginWrite(identity_);
  if (!transaction) {
    // Contended = another materialization or a live read lease (the
    // refusal-while-referenced rule); either way: eager-only.
    disarm("cache unavailable: " + transaction.error().message);
    return;
  }
  transaction_.emplace(std::move(*transaction));
  if (!writer_.open(transaction_->partialPath(), canonicalSourceDescriptorJson(descriptor), &error)) {
    disarm("artifact writer: " + error);
    transaction_.reset();
    return;
  }
  armed_ = true;
}

ArtifactCapture::~ArtifactCapture() {
  if (!finished_) {
    (void)finish(/*complete=*/false);  // abandoned session: abort + delete partial
  }
}

bool ArtifactCapture::armed() const {
  return armed_;
}

const std::string& ArtifactCapture::disarmReason() const {
  return disarm_reason_;
}

const std::string& ArtifactCapture::identity() const {
  return identity_;
}

const std::string& ArtifactCapture::descriptorJson() const {
  return descriptor_json_;
}

void ArtifactCapture::disarm(std::string reason) {
  if (armed_ || disarm_reason_.empty()) {
    disarm_reason_ = std::move(reason);
  }
  if (armed_) {
    writer_.abort();
    armed_ = false;
  }
}

void ArtifactCapture::captureScalarTopic(
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
    disarm("scalar capture (" + topic + "): " + error);
    return;
  }
  for (const auto& batch : batches) {
    if (!batch) {
      continue;
    }
    if (!writer_.writeBatch(*channel, *batch, batchLogTime(*batch, ts_field), &error)) {
      disarm("scalar capture (" + topic + "): " + error);
      return;
    }
  }
}

std::function<void(std::int64_t, const std::uint8_t*, std::size_t)> ArtifactCapture::objectSampleCapture(
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
            disarm("object capture (" + topic + "): " + error);
            return;
          }
          it = object_channels_.emplace(topic, *channel).first;
        }
        if (!writer_.writeObjectSample(it->second, ts_ns, data, size, &error)) {
          disarm("object capture (" + topic + "): " + error);
        }
      };
}

std::optional<ArtifactCapture::Finalized> ArtifactCapture::finish(bool complete) {
  finished_ = true;
  if (!armed_ || !complete) {
    if (armed_) {
      disarm(complete ? "finish failed" : "fetch incomplete");
    }
    // Abort eagerly: the writer has no valid footer and commit would reject
    // it anyway.
    if (transaction_.has_value()) {
      transaction_.reset();
    }
    return std::nullopt;
  }
  std::string error;
  if (!writer_.close(&error)) {
    disarm("artifact close: " + error);
    transaction_.reset();
    return std::nullopt;
  }
  auto committed = transaction_->commit();
  transaction_.reset();
  std::optional<PJ::sdk::descriptor_import::RequestArtifactCache::Hit> hit;
  if (committed) {
    hit.emplace(std::move(*committed));
  } else if (committed.error().retryable) {
    // commit() already published the artifact but lost the lease handoff to a
    // concurrent exclusive holder. Exactly one leased lookup is the SDK's
    // prescribed recovery; never return a bare, unpinned path.
    std::string miss_reason;
    hit = cache_.lookup(identity_, &miss_reason);
    if (!hit.has_value()) {
      disarm_reason_ = "cache commit lease recovery failed (" + committed.error().message + "): " + miss_reason;
      armed_ = false;
      return std::nullopt;
    }
  } else {
    disarm_reason_ = "cache commit: " + committed.error().message;
    armed_ = false;
    return std::nullopt;
  }
  armed_ = false;
  return Finalized{std::move(hit->path), std::move(hit->lease)};
}

}  // namespace mosaico
