// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Replays a Mosaico layout re-import cache artifact (Arrow-in-MCAP, produced
// by the Mosaico Cloud Server toolbox's cache tee) into the datastore:
// scalar topics re-append the exact stored Arrow batches, object topics
// re-push the exact stored canonical blobs at their exact timestamps —
// byte-identical to the original fetch by construction. Dialog-less on
// purpose: this loader is reached through source promotion and layout
// re-import (loaded by manifest id with a host-injected filepath), never by
// file drag-and-drop — its declared ".pjmosaico" extension is a marker that
// matches no real file, so it can never shadow the general MCAP loader.
#include <cstdint>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <vector>

#include "arrow_cache_artifact.hpp"
#include "artifact_replay.hpp"
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

    std::vector<mosaico::ArtifactTopicData> topics;
    std::string error;
    if (!mosaico::readArtifact(filepath, &topics, /*out_canonical_descriptor_json=*/nullptr, &error)) {
      return PJ::unexpected("cannot read cache artifact: " + error);
    }

    (void)runtimeHost().progressStart(
        "Replaying Mosaico cache", static_cast<uint64_t>(topics.size()), /*cancellable=*/true);

    // Object topics keep their host handles in a registry; the replay seam
    // hands out registry indices as its opaque handles.
    std::vector<PJ::sdk::ObjectTopicHandle> object_handles;

    mosaico::ReplaySinks sinks;
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
    sinks.progress = [this](std::size_t done, std::size_t /*total*/) {
      if (runtimeHost().isStopRequested()) {
        return false;
      }
      return runtimeHost().progressUpdate(static_cast<uint64_t>(done));
    };

    if (!mosaico::replayArtifact(topics, sinks, &error)) {
      // A user stop keeps what already replayed (the host's keep/discard
      // choice applies); a genuine failure aborts.
      if (runtimeHost().isStopRequested()) {
        return PJ::okStatus();
      }
      return PJ::unexpected("cache replay failed: " + error);
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "Replayed " + std::to_string(topics.size()) + " topic(s) from the Mosaico cache");
    return PJ::okStatus();
  }

 private:
  std::string config_json_ = "{}";
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(MosaicoCacheSource, kMosaicoCacheManifest)
