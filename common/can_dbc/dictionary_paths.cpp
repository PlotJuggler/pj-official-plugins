#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <pj_can_dbc/dictionary_paths.hpp>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace pj_can_dbc {

namespace {

namespace fs = std::filesystem;

fs::path toPath(std::string_view utf8) {
  return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string toUtf8(const fs::path& path) {
  const auto text = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool isUrl(std::string_view text) {
  return text.find("://") != std::string_view::npos;
}

/// The lexically normal directory of data_file; empty for an empty or URL data file.
fs::path dataDir(std::string_view data_file) {
  if (data_file.empty() || isUrl(data_file)) {
    return {};
  }
  return toPath(data_file).lexically_normal().parent_path();
}

/// True when `path` has more components than `dir` and starts with all of them.
bool isUnder(const fs::path& path, const fs::path& dir) {
  const auto [dir_end, path_end] = std::mismatch(dir.begin(), dir.end(), path.begin(), path.end());
  return dir_end == dir.end() && path_end != path.end();
}

/// Components of `path` (a vector: fs::path::iterator stashes its element, so reverse iterators over it are UB).
std::vector<fs::path> componentsOf(const fs::path& path) {
  return std::vector<fs::path>(path.begin(), path.end());
}

/// How many trailing components can be stripped from `path` while keeping the root and one component below it.
std::ptrdiff_t maxStrip(const fs::path& path, const std::vector<fs::path>& parts) {
  const std::ptrdiff_t root_parts = (path.has_root_name() ? 1 : 0) + (path.has_root_directory() ? 1 : 0);
  return static_cast<std::ptrdiff_t>(parts.size()) - root_parts - 1;
}

fs::path stripLast(fs::path path, std::ptrdiff_t count) {
  for (; count > 0; --count) {
    path = path.parent_path();
  }
  return path;
}

/// The (old, new) folder pair a layout moved between, from the two data-file paths. The common trailing
/// components are the part inside the moved folder; stripping them from each path gives the old and new roots.
/// Example: old/A/data/run.blf -> new/B/data/run.blf share "data/run.blf", so old/A moved to new/B and a sibling
/// old/A/dbc/v.dbc follows. The strip count is clamped to [1, what keeps the root and one component below it], so
/// a renamed data file (nothing in common) strips just the file name and the two data directories are the roots,
/// and a common part that covers the whole path never reaches the filesystem root.
std::optional<std::pair<fs::path, fs::path>> movedRoots(std::string_view saved_data_file, std::string_view data_file) {
  if (saved_data_file.empty() || data_file.empty() || isUrl(saved_data_file) || isUrl(data_file)) {
    return std::nullopt;
  }
  const fs::path old_file = toPath(saved_data_file).lexically_normal();
  const fs::path new_file = toPath(data_file).lexically_normal();
  if (!old_file.is_absolute() || !new_file.is_absolute()) {
    return std::nullopt;
  }
  const auto old_parts = componentsOf(old_file);
  const auto new_parts = componentsOf(new_file);
  const std::ptrdiff_t common =
      std::mismatch(old_parts.rbegin(), old_parts.rend(), new_parts.rbegin(), new_parts.rend()).first -
      old_parts.rbegin();
  const std::ptrdiff_t limit = std::min(maxStrip(old_file, old_parts), maxStrip(new_file, new_parts));
  const std::ptrdiff_t strip = limit < 1 ? 1 : std::clamp<std::ptrdiff_t>(common, 1, limit);
  return std::make_pair(stripLast(old_file, strip), stripLast(new_file, strip));
}

bool isRegularFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

}  // namespace

std::string toConfigPath(std::string_view aux_path, std::string_view data_file) {
  if (isUrl(aux_path)) {
    return std::string(aux_path);
  }
  const fs::path aux = toPath(aux_path);
  const fs::path dir = dataDir(data_file);
  if (!aux.is_absolute() || !dir.is_absolute()) {
    return std::string(aux_path);
  }
  const fs::path normal = aux.lexically_normal();
  if (!isUnder(normal, dir)) {
    return std::string(aux_path);
  }
  return toUtf8(normal.lexically_relative(dir));
}

std::string auxBaseDir(std::string_view data_file) {
  const fs::path dir = dataDir(data_file);
  return dir.is_absolute() ? toUtf8(dir) : std::string();
}

std::string resolveAuxPath(std::string_view stored, const AuxPathContext& context) {
  if (stored.empty() || isUrl(stored) || context.data_file.empty() || isUrl(context.data_file)) {
    return std::string(stored);
  }
  const fs::path path = toPath(stored);
  const fs::path current = dataDir(context.data_file);
  if (path.is_relative()) {
    const bool use_saved_base =
        context.saved_data_file.empty() && !context.base_dir.empty() && !isUrl(context.base_dir);
    const fs::path base = use_saved_base ? toPath(context.base_dir) : current;
    return toUtf8((base / path).lexically_normal());
  }
  const auto roots = movedRoots(context.saved_data_file, context.data_file);
  const fs::path normal = path.lexically_normal();
  if (!roots || !isUnder(normal, roots->first)) {
    return std::string(stored);
  }
  const fs::path rebased = (roots->second / normal.lexically_relative(roots->first)).lexically_normal();
  // The moved-folder guess can be wrong for a dictionary that never moved (a shared library elsewhere), so the
  // stored file is the fallback; when both exist (a copied layout) the copy's file wins.
  if (!isRegularFile(rebased) && isRegularFile(path)) {
    return std::string(stored);
  }
  return toUtf8(rebased);
}

std::vector<std::string> toConfigPaths(const std::vector<std::string>& paths, std::string_view data_file) {
  std::vector<std::string> result;
  result.reserve(paths.size());
  for (const auto& path : paths) {
    result.push_back(toConfigPath(path, data_file));
  }
  return result;
}

std::vector<std::string> resolveAuxPaths(const std::vector<std::string>& stored, const AuxPathContext& context) {
  std::vector<std::string> result;
  result.reserve(stored.size());
  for (const auto& path : stored) {
    result.push_back(resolveAuxPath(path, context));
  }
  return result;
}

PJ::Expected<std::string> readAuxFile(const std::string& path, std::string_view kind) {
  const fs::path file = toPath(path);
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    return PJ::unexpected(std::string(kind) + " file not found: " + path);
  }
  std::ifstream stream(file, std::ios::binary);
  if (!stream.is_open()) {
    return PJ::unexpected("cannot read " + std::string(kind) + " file: " + path);
  }
  std::ostringstream text;
  text << stream.rdbuf();
  if (stream.bad()) {
    return PJ::unexpected("cannot read " + std::string(kind) + " file: " + path);
  }
  return text.str();
}

}  // namespace pj_can_dbc
