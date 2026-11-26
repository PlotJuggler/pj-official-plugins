// SPDX-License-Identifier: MIT
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/mesh3d_codec.hpp>
#include <pj_base/builtin/point_cloud_codec.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <vector>

#include "cloud_common.hpp"
#include "data_load_3d_manifest.hpp"
#include "las_reader.hpp"
#include "loader_helpers.hpp"
#include "pcd_reader.hpp"
#include "ply_reader.hpp"

namespace {

class Load3dSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) {
      return PJ::unexpected(std::string("invalid 3D-loader config JSON"));
    }
    auto it = cfg.find("filepath");
    if (it == cfg.end() || !it->is_string()) {
      return PJ::unexpected(std::string("3D-loader config missing or non-string `filepath` field"));
    }
    filepath_ = it->get<std::string>();
    if (filepath_.empty()) {
      return PJ::unexpected(std::string("3D-loader config `filepath` is empty"));
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    std::ifstream in(filepath_, std::ios::binary);
    if (!in) {
      return PJ::unexpected("cannot open file: " + filepath_);
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.empty()) {
      return PJ::unexpected("file is empty: " + filepath_);
    }
    const std::string stem = pj3d::fileStem(filepath_);
    const std::string ext = pj3d::lowerExtension(filepath_);
    PJ::Span<const uint8_t> bytes(raw.data(), raw.size());

    if (ext == ".pcd") {
      auto built = pj3d::readPcd(bytes, stem);
      if (!built) {
        return PJ::unexpected(built.error());
      }
      return emitCloud(stem, *built);
    }
    if (ext == ".ply") {
      auto r = pj3d::readPly(bytes, stem);
      if (!r) {
        return PJ::unexpected(r.error());
      }
      if (r->cloud) {
        return emitCloud(stem, *r->cloud);
      }
      return emitMesh(stem, *r->mesh, r->num_faces);
    }
    if (ext == ".las") {
      auto r = pj3d::readLas(bytes, stem);
      if (!r) {
        return PJ::unexpected(r.error());
      }
      std::vector<uint8_t>().swap(raw);  // the cloud owns a copy; LAS files reach GBs
      return emitCloud(stem, r->cloud, r->origin);
    }
    return PJ::unexpected("unsupported extension for 3D loader: " + ext);
  }

 private:
  // Register <stem> as an object topic of `type` and push the serialized bytes
  // at t=0. When no object-write host is bound, warn and no-op (the caller still
  // emits the scalar summary). Returns an error only if a bound host rejects.
  PJ::Status pushObject(const std::string& stem, PJ::sdk::BuiltinObjectType type, PJ::Span<const uint8_t> payload) {
    const auto* owh = objectWriteHost();
    if (owh == nullptr) {
      warnNoObjectHost();
      return PJ::okStatus();
    }
    auto handle = owh->registerTopic(stem, pj3d::builtinObjectMetadata(type));
    if (!handle) {
      return PJ::unexpected(handle.error());
    }
    return owh->pushOwned(*handle, PJ::Timestamp{0}, payload);
  }

  // `origin` is what the reader subtracted from the file coordinates (LAS only);
  // it is published in the summary so the user can recover them.
  PJ::Status emitCloud(
      const std::string& stem, const PJ::sdk::PointCloud& cloud,
      const std::optional<std::array<double, 3>>& origin = std::nullopt) {
    const uint64_t num_points = static_cast<uint64_t>(cloud.width) * static_cast<uint64_t>(cloud.height);
    auto payload = PJ::serializePointCloud(cloud);
    // Note: object push and summary emit are not transactional; on a summary
    // failure after a successful object push, importData returns an error and
    // the partial entry is abandoned (the user retries the load).
    if (auto st = pushObject(
            stem, PJ::sdk::BuiltinObjectType::kPointCloud, PJ::Span<const uint8_t>(payload.data(), payload.size()));
        !st) {
      return st;
    }
    auto centroid = pj3d::computeCentroid(cloud);
    return emitSummary(stem, num_points, /*num_faces=*/0, centroid, origin);
  }

  PJ::Status emitMesh(const std::string& stem, const PJ::sdk::Mesh3D& mesh, uint64_t num_faces) {
    auto payload = PJ::serializeMesh3D(mesh);
    if (auto st = pushObject(
            stem, PJ::sdk::BuiltinObjectType::kMesh3D, PJ::Span<const uint8_t>(payload.data(), payload.size()));
        !st) {
      return st;
    }
    return emitSummary(stem, /*num_points=*/0, num_faces, std::nullopt, std::nullopt);
  }

  PJ::Status emitSummary(
      const std::string& stem, uint64_t num_points, uint64_t num_faces,
      const std::optional<std::array<double, 3>>& centroid, const std::optional<std::array<double, 3>>& origin) {
    auto topic = writeHost().ensureTopic(stem + "/summary");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    std::vector<PJ::sdk::NamedFieldValue> fields;
    // num_points is a cloud metric; a mesh reports num_faces instead. Clouds
    // always have num_faces == 0 (a positive face count routes to the mesh path).
    if (num_faces == 0) {
      fields.push_back({"num_points", PJ::sdk::ValueRef{num_points}});  // num_points is uint64_t
    }
    if (num_faces > 0) {
      fields.push_back({"num_faces", PJ::sdk::ValueRef{num_faces}});  // uint64_t
    }
    if (centroid.has_value()) {
      fields.push_back({"centroid/x", PJ::sdk::ValueRef{(*centroid)[0]}});
      fields.push_back({"centroid/y", PJ::sdk::ValueRef{(*centroid)[1]}});
      fields.push_back({"centroid/z", PJ::sdk::ValueRef{(*centroid)[2]}});
    }
    if (origin.has_value()) {
      fields.push_back({"origin/x", PJ::sdk::ValueRef{(*origin)[0]}});
      fields.push_back({"origin/y", PJ::sdk::ValueRef{(*origin)[1]}});
      fields.push_back({"origin/z", PJ::sdk::ValueRef{(*origin)[2]}});
    }
    return writeHost().appendRecord(
        *topic, PJ::Timestamp{0}, PJ::Span<const PJ::sdk::NamedFieldValue>(fields.data(), fields.size()));
  }

  void warnNoObjectHost() {
    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kWarning, "ObjectStore write host unavailable; imported scalar summary only");
  }

  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(Load3dSource, kDataLoad3dManifest)
