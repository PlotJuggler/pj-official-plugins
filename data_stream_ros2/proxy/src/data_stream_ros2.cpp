/**
 * @file data_stream_ros2.cpp
 * @brief ROS 2 topic subscriber plugin — distro dispatch entry point.
 *
 * The host plugin loader opens this shared library directly (it is the
 * entry point of the marketplace extension). On the first call to
 * PJ_get_data_source_vtable() or PJ_get_dialog_vtable() it detects the
 * ROS 2 distribution installed on the user's system, dlopen-s the
 * matching per-distro binary (`dist/<distro>/libros2_stream_plugin-<distro>.so`
 * relative to this library), resolves the inner's *private* getters, and
 * returns the resulting vtables. From that point onward the host drives
 * the inner library directly — this dispatch layer only participates in
 * the initial resolution.
 *
 * The inner DSO deliberately does NOT export the standard
 * `PJ_get_data_source_vtable` / `PJ_get_dialog_vtable` symbols — the
 * host's recursive plugin scanner (`scanPluginDsos`) would otherwise
 * register every per-distro inner as a top-level plugin (one per ROS
 * distro) instead of just this proxy. Inner exports are therefore named
 * `PJ_ros2_inner_get_*_vtable`, invisible to the scanner but reachable
 * from this file via dlsym.
 *
 * By design this translation unit has **no dependency** on rclcpp or any
 * ROS component. Only <dlfcn.h>, <filesystem> and <cstdlib>. This way
 * the proxy loads cleanly on machines without ROS installed, so we can
 * report a clear error instead of a dynamic-linker failure.
 */

#include <dlfcn.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "pj_base/data_source_protocol.h"
#include "pj_base/plugin_abi_export.hpp"
#include "pj_plugins/dialog_protocol.h"

namespace {

// Supported ROS 2 distributions, in preferred order. When the user has
// several installed and no explicit ROS_DISTRO env var, the proxy picks the
// highest-priority one present. LTS distros come first because they are the
// reasonable "default" for a user who hasn't sourced anything.
constexpr std::array<std::string_view, 4> kSupportedDistros = {"humble", "iron", "jazzy", "rolling"};

// Cached state — the inner library stays resident for the process lifetime.
std::once_flag g_load_once;
void* g_inner_handle = nullptr;
const PJ_data_source_vtable_t* g_data_source_vtable = nullptr;
const PJ_dialog_vtable_t* g_dialog_vtable = nullptr;
std::string g_last_error;

// Returns the directory that contains this .so. Used as anchor to
// locate the sibling `dist/` folder with per-distro binaries.
std::filesystem::path proxyDirectory() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&proxyDirectory), &info) == 0 || info.dli_fname == nullptr) {
    return {};
  }
  return std::filesystem::path(info.dli_fname).parent_path();
}

bool distroDirectoryExists(std::string_view distro) {
  return std::filesystem::exists("/opt/ros/" + std::string(distro) + "/setup.bash");
}

bool condaDistroDirectoryExists(const std::filesystem::path& conda_prefix, std::string_view distro) {
  return std::filesystem::exists(conda_prefix / "share" / std::string(distro));
}

// Detect the ROS distribution to dispatch to. Chain of methods, highest
// reliability first. See 2026.04.23-ros2-proxy-dispatch-study.md for the
// rationale behind each.
std::optional<std::string> detectRosDistro() {
  // 1. Explicit env var — set by setup.bash. Gold standard.
  if (const char* env = std::getenv("ROS_DISTRO"); env != nullptr && *env != '\0') {
    return std::string(env);
  }

  // 2. Standard apt/debian install: /opt/ros/<distro>/setup.bash
  for (const auto& distro : kSupportedDistros) {
    if (distroDirectoryExists(distro)) {
      return std::string(distro);
    }
  }

  // 3. RoboStack / conda overlay: $CONDA_PREFIX/share/<distro>
  if (const char* prefix = std::getenv("CONDA_PREFIX"); prefix != nullptr && *prefix != '\0') {
    const std::filesystem::path conda_prefix(prefix);
    for (const auto& distro : kSupportedDistros) {
      if (condaDistroDirectoryExists(conda_prefix, distro)) {
        return std::string(distro);
      }
    }
  }

  return std::nullopt;
}

std::string supportedDistrosList() {
  std::string out;
  for (const auto& d : kSupportedDistros) {
    if (!out.empty()) {
      out += ", ";
    }
    out += d;
  }
  return out;
}

// Called exactly once. Resolves the distro, loads the inner library, captures
// its data-source and dialog vtables. On failure populates g_last_error and
// leaves the cached vtables null.
void loadInnerOnce() {
  auto distro = detectRosDistro();
  if (!distro.has_value()) {
    g_last_error =
        "ROS 2 distribution not detected. "
        "Source /opt/ros/<distro>/setup.bash before launching PlotJuggler, "
        "or install one of: " +
        supportedDistrosList() + ".";
    return;
  }

  auto dir = proxyDirectory();
  if (dir.empty()) {
    g_last_error = "Unable to locate proxy shared library directory (dladdr failed).";
    return;
  }

  const auto inner_path = dir / "dist" / *distro / ("libros2_stream_plugin-" + *distro + ".so");
  if (!std::filesystem::exists(inner_path)) {
    g_last_error = "ROS 2 distribution '" + *distro +
                   "' is installed but no matching binary is shipped in this extension. "
                   "Supported: " +
                   supportedDistrosList() + ".";
    return;
  }

  // RTLD_LOCAL keeps rclcpp symbols from leaking to other plugins; RTLD_LAZY
  // defers symbol resolution so incompatible leaves don't blow up on load.
  g_inner_handle = dlopen(inner_path.string().c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (g_inner_handle == nullptr) {
    const char* err = dlerror();
    g_last_error = "dlopen failed for " + inner_path.string() + ": " + (err != nullptr ? err : "unknown");
    return;
  }

  using DataSourceGetter = const PJ_data_source_vtable_t* (*)(void);
  auto* ds_getter = reinterpret_cast<DataSourceGetter>(dlsym(g_inner_handle, "PJ_ros2_inner_get_data_source_vtable"));
  if (ds_getter == nullptr) {
    const char* err = dlerror();
    g_last_error =
        "dlsym(PJ_ros2_inner_get_data_source_vtable) failed: " + std::string(err != nullptr ? err : "unknown");
    dlclose(g_inner_handle);
    g_inner_handle = nullptr;
    return;
  }

  using DialogGetter = const PJ_dialog_vtable_t* (*)(void);
  auto* dlg_getter = reinterpret_cast<DialogGetter>(dlsym(g_inner_handle, "PJ_ros2_inner_get_dialog_vtable"));
  if (dlg_getter == nullptr) {
    const char* err = dlerror();
    g_last_error = "dlsym(PJ_ros2_inner_get_dialog_vtable) failed: " + std::string(err != nullptr ? err : "unknown");
    dlclose(g_inner_handle);
    g_inner_handle = nullptr;
    return;
  }

  const PJ_data_source_vtable_t* ds_vt = ds_getter();
  if (ds_vt == nullptr) {
    g_last_error = "Inner data-source getter returned a null vtable.";
    dlclose(g_inner_handle);
    g_inner_handle = nullptr;
    return;
  }

  const PJ_dialog_vtable_t* dlg_vt = dlg_getter();
  if (dlg_vt == nullptr) {
    g_last_error = "Inner dialog getter returned a null vtable.";
    dlclose(g_inner_handle);
    g_inner_handle = nullptr;
    return;
  }

  g_data_source_vtable = ds_vt;
  g_dialog_vtable = dlg_vt;
}

}  // namespace

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() {
  std::call_once(g_load_once, loadInnerOnce);
  return g_data_source_vtable;
}

extern "C" PJ_DIALOG_EXPORT const PJ_dialog_vtable_t* PJ_get_dialog_vtable() {
  std::call_once(g_load_once, loadInnerOnce);
  return g_dialog_vtable;
}

/// Diagnostic hook: the host may query this if a vtable getter returned
/// null, to show a human-readable reason to the user. Optional — older hosts
/// that don't know about it simply ignore the symbol.
extern "C" PJ_DATA_SOURCE_EXPORT const char* PJ_get_proxy_last_error() {
  return g_last_error.empty() ? nullptr : g_last_error.c_str();
}
