// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cctype>
#include <pj_base/builtin/builtin_object.hpp>
#include <string>

namespace pj3d {

// {"builtin_object_type":"kPointCloud"} / {"builtin_object_type":"kMesh3D"} —
// PJ4 classifies an ObjectStore topic for rendering by this key (NOT media_class).
inline std::string builtinObjectMetadata(PJ::sdk::BuiltinObjectType type) {
  return std::string(R"({"builtin_object_type":")") + std::string(PJ::sdk::name(type)) + R"("})";
}

// Lowercased extension including the dot, e.g. ".pcd". Empty if none.
inline std::string lowerExtension(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    return {};
  }
  std::string ext = path.substr(dot);
  std::transform(
      ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

// File stem: base name without directory or extension.
inline std::string fileStem(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
  if (end == start) {  // dotfile like ".pcd" -> use the whole basename, not ""
    end = path.size();
  }
  return path.substr(start, end - start);
}

}  // namespace pj3d
