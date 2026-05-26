#include "dataset_model.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

namespace lerobot {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr int kMaxRootWalk = 6;
constexpr const char* kDefaultDataPath = "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet";
constexpr const char* kDefaultVideoPath =
    "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4";

std::string zeroPad(int64_t value, int width) {
  std::string digits = std::to_string(value < 0 ? -value : value);
  if (static_cast<int>(digits.size()) < width) {
    digits.insert(0, static_cast<std::size_t>(width) - digits.size(), '0');
  }
  return value < 0 ? "-" + digits : digits;
}

// Parse a Python-style format spec like "03d" / "06d" → field width (0 = none).
int parseWidth(std::string_view spec) {
  int width = 0;
  for (char c : spec) {
    if (c >= '0' && c <= '9') {
      width = width * 10 + (c - '0');
    }
  }
  return width;
}

std::string strField(const json& obj, const char* key, std::string_view fallback = {}) {
  auto it = obj.find(key);
  if (it != obj.end() && it->is_string()) {
    return it->get<std::string>();
  }
  return std::string(fallback);
}

int64_t intField(const json& obj, const char* key, int64_t fallback) {
  auto it = obj.find(key);
  if (it != obj.end() && it->is_number_integer()) {
    return it->get<int64_t>();
  }
  return fallback;
}

double doubleField(const json& obj, const char* key, double fallback) {
  auto it = obj.find(key);
  if (it != obj.end() && it->is_number()) {
    return it->get<double>();
  }
  return fallback;
}

PJ::Expected<json> readJsonFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return PJ::unexpected("cannot open " + path.string());
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  json parsed = json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return PJ::unexpected("invalid JSON in " + path.string());
  }
  return parsed;
}

// Classify the LeRobot dataset version from info.json's `codebase_version`.
// v2.0 / v2.1 share the same on-disk layout from the plugin's point of view
// and collapse to `V2_x`. Any `v3.x` is accepted as `V3_0` (the v3 line
// reserves further point releases for non-breaking metadata extensions; the
// plugin reads only the fields documented for v3.0 and is forward-tolerant
// of new ones). Everything else is rejected.
PJ::Expected<DatasetVersion> gateVersion(const std::string& version) {
  if (version == "v2.0" || version == "v2.1") {
    return DatasetVersion::V2_x;
  }
  std::string trimmed = version;
  if (!trimmed.empty() && (trimmed[0] == 'v' || trimmed[0] == 'V')) {
    trimmed.erase(0, 1);
  }
  int major = 0;
  for (char c : trimmed) {
    if (c == '.') {
      break;
    }
    if (c >= '0' && c <= '9') {
      major = major * 10 + (c - '0');
    }
  }
  if (major == 3) {
    return DatasetVersion::V3_0;
  }
  if (major >= 4) {
    return PJ::unexpected(
        "LeRobot v" + std::to_string(major) + ".x datasets are not supported (found codebase_version='" + version +
        "'); this plugin understands v2.0, v2.1 and v3.x");
  }
  return PJ::unexpected("unsupported LeRobot codebase_version='" + version + "' (expected v2.0, v2.1 or v3.x)");
}

PJ::Expected<fs::path> resolveRoot(const fs::path& picked) {
  std::error_code ec;
  fs::path start = fs::absolute(picked, ec);
  if (ec) {
    start = picked;
  }
  // If a file was picked, begin the walk from its directory.
  if (fs::is_regular_file(start, ec)) {
    start = start.parent_path();
  } else if (!fs::is_directory(start, ec)) {
    start = start.parent_path();
  }
  fs::path dir = start;
  for (int i = 0; i < kMaxRootWalk && !dir.empty(); ++i) {
    if (fs::is_regular_file(dir / "meta" / "info.json", ec)) {
      return dir;
    }
    if (dir == dir.parent_path()) {
      break;
    }
    dir = dir.parent_path();
  }
  return PJ::unexpected("not a LeRobot dataset: meta/info.json not found near " + picked.string());
}

// `names` may be a flat array (["a","b"]) or a dict of arrays
// (e.g. {"motors": ["motor_0","motor_1"]}, as used by lerobot/pusht).
// Collect the string leaves in document order, recursing through
// arrays/objects.
void collectNameLeaves(const json& node, std::vector<std::string>& out) {
  if (node.is_string()) {
    out.push_back(node.get<std::string>());
  } else if (node.is_array()) {
    for (const auto& e : node) {
      collectNameLeaves(e, out);
    }
  } else if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      collectNameLeaves(it.value(), out);
    }
  }
}

void parseFeatures(const json& info, DatasetModel& model) {
  auto feats = info.find("features");
  if (feats == info.end() || !feats->is_object()) {
    return;
  }
  for (auto it = feats->begin(); it != feats->end(); ++it) {
    const json& f = it.value();
    FeatureSpec spec;
    spec.name = it.key();
    spec.dtype = strField(f, "dtype");
    if (auto sh = f.find("shape"); sh != f.end() && sh->is_array()) {
      for (const auto& d : *sh) {
        if (d.is_number_integer()) {
          spec.shape.push_back(d.get<int64_t>());
        }
      }
    }
    if (auto nm = f.find("names"); nm != f.end()) {
      collectNameLeaves(*nm, spec.names);
    }
    if (spec.is_video()) {
      model.camera_names.push_back(spec.name);
    }
    model.features.push_back(std::move(spec));
  }
}

PJ::Status parseTasks(const fs::path& root, std::vector<std::string>& out_by_index) {
  fs::path tasks_path = root / "meta" / "tasks.jsonl";
  std::ifstream in(tasks_path, std::ios::binary);
  if (!in) {
    return PJ::okStatus();  // tasks.jsonl is optional
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      continue;
    }
    int64_t idx = intField(j, "task_index", -1);
    std::string task = strField(j, "task");
    if (idx < 0) {
      continue;
    }
    if (static_cast<int64_t>(out_by_index.size()) <= idx) {
      out_by_index.resize(static_cast<std::size_t>(idx) + 1);
    }
    out_by_index[static_cast<std::size_t>(idx)] = std::move(task);
  }
  return PJ::okStatus();
}

PJ::Status parseEpisodes(const fs::path& root, const std::vector<std::string>& tasks_by_index, DatasetModel& model) {
  fs::path ep_path = root / "meta" / "episodes.jsonl";
  std::ifstream in(ep_path, std::ios::binary);
  if (!in) {
    return PJ::unexpected("missing " + ep_path.string());
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      continue;
    }
    EpisodeInfo ep;
    ep.episode_index = intField(j, "episode_index", 0);
    ep.length = intField(j, "length", 0);
    if (auto t = j.find("tasks"); t != j.end() && t->is_array() && !t->empty()) {
      const json& first = (*t)[0];
      if (first.is_string()) {
        ep.task_text = first.get<std::string>();
      } else if (first.is_number_integer()) {
        auto idx = first.get<int64_t>();
        if (idx >= 0 && idx < static_cast<int64_t>(tasks_by_index.size())) {
          ep.task_text = tasks_by_index[static_cast<std::size_t>(idx)];
        }
      }
    }
    model.episodes.push_back(std::move(ep));
  }
  std::sort(model.episodes.begin(), model.episodes.end(), [](const EpisodeInfo& a, const EpisodeInfo& b) {
    return a.episode_index < b.episode_index;
  });
  return PJ::okStatus();
}

}  // namespace

std::string expandPathTemplate(
    std::string_view tmpl, int64_t episode_chunk, int64_t episode_index, std::string_view video_key) {
  std::string out;
  out.reserve(tmpl.size() + 16);
  for (std::size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] != '{') {
      out.push_back(tmpl[i++]);
      continue;
    }
    std::size_t close = tmpl.find('}', i);
    if (close == std::string_view::npos) {
      out.append(tmpl.substr(i));
      break;
    }
    std::string_view token = tmpl.substr(i + 1, close - i - 1);
    std::string_view name = token;
    std::string_view spec;
    if (auto colon = token.find(':'); colon != std::string_view::npos) {
      name = token.substr(0, colon);
      spec = token.substr(colon + 1);
    }
    if (name == "episode_chunk") {
      out += zeroPad(episode_chunk, parseWidth(spec));
    } else if (name == "episode_index") {
      out += zeroPad(episode_index, parseWidth(spec));
    } else if (name == "video_key" || name == "camera_key") {
      out.append(video_key);
    } else {
      out.append(tmpl.substr(i, close - i + 1));  // unknown token: keep literal
    }
    i = close + 1;
  }
  return out;
}

const FeatureSpec* DatasetModel::feature(std::string_view name) const {
  for (const auto& f : features) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

std::filesystem::path DatasetModel::episodeParquet(int64_t episode_index) const {
  int64_t chunk = chunks_size > 0 ? episode_index / chunks_size : 0;
  return root / expandPathTemplate(data_path_tmpl, chunk, episode_index, {});
}

std::filesystem::path DatasetModel::episodeVideo(int64_t episode_index, std::string_view camera) const {
  int64_t chunk = chunks_size > 0 ? episode_index / chunks_size : 0;
  return root / expandPathTemplate(video_path_tmpl, chunk, episode_index, camera);
}

PJ::Expected<DatasetModel> loadDatasetModel(const std::filesystem::path& picked_path) {
  auto root = resolveRoot(picked_path);
  if (!root) {
    return PJ::unexpected(root.error());
  }

  auto info_or = readJsonFile(*root / "meta" / "info.json");
  if (!info_or) {
    return PJ::unexpected(info_or.error());
  }
  const json& info = *info_or;
  if (!info.is_object()) {
    return PJ::unexpected("meta/info.json is not a JSON object");
  }

  DatasetModel model;
  model.root = *root;
  model.codebase_version = strField(info, "codebase_version");
  auto ver_or = gateVersion(model.codebase_version);
  if (!ver_or) {
    return PJ::unexpected(ver_or.error());
  }
  model.version = *ver_or;
  model.fps = doubleField(info, "fps", 30.0);
  model.chunks_size = intField(info, "chunks_size", 1000);
  model.data_path_tmpl = strField(info, "data_path", kDefaultDataPath);
  model.video_path_tmpl = strField(info, "video_path", kDefaultVideoPath);

  parseFeatures(info, model);

  std::vector<std::string> tasks_by_index;
  if (auto status = parseTasks(*root, tasks_by_index); !status) {
    return PJ::unexpected(status.error());
  }
  if (auto status = parseEpisodes(*root, tasks_by_index, model); !status) {
    return PJ::unexpected(status.error());
  }
  if (model.episodes.empty()) {
    return PJ::unexpected("no episodes found in meta/episodes.jsonl");
  }
  return model;
}

}  // namespace lerobot
