// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Replays a Mosaico layout re-import cache artifact (Arrow-in-MCAP, produced
// by the Mosaico Cloud Server toolbox's artifact capture) into the datastore:
// scalar topics re-append the exact stored Arrow batches, object topics
// re-push the exact stored canonical blobs at their exact timestamps —
// byte-identical to the original fetch by construction. Dialog-less on
// purpose: this loader is reached through source promotion and layout
// re-import (loaded by manifest id with a host-injected filepath), never by
// file drag-and-drop — its declared ".pjmosaico" extension is a marker that
// matches no real file, so it can never shadow the general MCAP loader.
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <vector>

#include "descriptor_import/arrow_cache_artifact.hpp"
#include "descriptor_import/artifact_replay.hpp"
#include "mosaico_cache_manifest.hpp"

namespace {

class MosaicoCacheSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return config_json_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    const auto cfg = nlohmann::json::parse(config_json, nullptr, /*allow_exceptions=*/false);
    if (cfg.is_discarded() || !cfg.is_object()) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    config_json_ = std::string(config_json);
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    std::string filepath;
    {
      const auto cfg = nlohmann::json::parse(config_json_, nullptr, /*allow_exceptions=*/false);
      if (!cfg.is_discarded() && cfg.is_object()) {
        filepath = cfg.value("filepath", std::string{});
      }
    }
    if (filepath.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    // Bounded-budget preflight BEFORE the full parse: the artifact's filename
    // stem IS its identity digest (<32hex>.pjmosaico), so validateArtifact can
    // re-hash the embedded provenance against it — a corrupted, replaced, or
    // renamed file fails here instead of driving the reader on foreign bytes.
    const std::string stem = std::filesystem::path(filepath).stem().string();
    std::string error;
    // Run the allocation-safe raw framing gate before the provenance
    // validator enters MCAP: a forged Header or Chunk-in-summary must never
    // reach an allocating parser in this production path.
    if (!mosaico::preflightArtifactForReplay(filepath, &error)) {
      return PJ::unexpected("cache artifact failed replay preflight: " + error);
    }
    if (!mosaico::validateArtifact(filepath, stem, &error)) {
      return PJ::unexpected("cache artifact failed validation: " + error);
    }
    // Object topics keep their host handles in a registry; the replay seam
    // hands out registry indices as its opaque handles.
    std::vector<PJ::sdk::ObjectTopicHandle> object_handles;
    std::size_t topic_count = 0;

    mosaico::ReplaySinks sinks;
    sinks.start = [this, &topic_count](
                      std::size_t topics_total, std::uint64_t messages_total, std::string* start_error) {
      topic_count = topics_total;
      const auto status = runtimeHost().progressStart("Replaying Mosaico cache", messages_total, /*cancellable=*/true);
      if (!status) {
        *start_error = status.error();
        return false;
      }
      return true;
    };
    sinks.append_scalar_stream = [this](
                                     const std::string& topic, ArrowArrayStream* stream,
                                     const std::string& timestamp_column, std::string* sink_error) {
      const auto handle = writeHost().ensureTopic(topic);
      if (!handle) {
        // Failure = ownership NOT transferred; this sink owns the release.
        if (stream->release != nullptr) {
          stream->release(stream);
        }
        *sink_error = handle.error();
        return false;
      }
      const auto status = writeHost().appendArrowStream(*handle, stream, timestamp_column);
      if (!status) {
        if (stream->release != nullptr) {
          stream->release(stream);
        }
        *sink_error = status.error();
        return false;
      }
      return true;
    };
    sinks.register_object_topic = [this, &object_handles](
                                      const std::string& topic, const std::string& metadata_json,
                                      std::string* sink_error) -> std::optional<std::uint64_t> {
      const auto* object_host = objectWriteHost();
      if (object_host == nullptr || !object_host->valid()) {
        *sink_error = "host provides no object write surface";
        return std::nullopt;
      }
      const auto handle = object_host->registerTopic(topic, metadata_json);
      if (!handle) {
        *sink_error = handle.error();
        return std::nullopt;
      }
      object_handles.push_back(*handle);
      return object_handles.size() - 1;
    };
    sinks.push_object = [this, &object_handles](
                            std::uint64_t handle, std::int64_t ts_ns, const std::uint8_t* data, std::size_t size,
                            std::string* sink_error) {
      const auto status = objectWriteHost()->pushOwned(
          object_handles.at(handle), PJ::Timestamp{ts_ns}, PJ::Span<const uint8_t>(data, size));
      if (!status) {
        *sink_error = status.error();
        return false;
      }
      return true;
    };
    sinks.is_cancelled = [this]() { return runtimeHost().isStopRequested(); };
    sinks.progress = [this](std::uint64_t done, std::uint64_t /*total*/) { return runtimeHost().progressUpdate(done); };

    if (!mosaico::replayArtifact(filepath, sinks, &error)) {
      if (mosaico::isReplayCancellation(error, runtimeHost().isStopRequested())) {
        return PJ::unexpected(std::string("import cancelled"));
      }
      return PJ::unexpected("cache replay failed: " + error);
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "Replayed " + std::to_string(topic_count) + " topic(s) from the Mosaico cache");
    return PJ::okStatus();
  }

 private:
  std::string config_json_ = "{}";
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(MosaicoCacheSource, kMosaicoCacheManifest)
