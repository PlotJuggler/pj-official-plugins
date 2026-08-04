/**
 * @file data_stream_ros2.cpp
 * @brief ROS 2 topic subscriber plugin — distro dispatch entry point.
 *
 * The host plugin loader opens this shared library directly (it is the
 * entry point of the marketplace extension). `PJ_get_data_source_vtable()`
 * returns a *static* proxy vtable that is self-describing: it carries this
 * extension's embedded manifest, so the host's plugin scanner can discover
 * and catalog the extension on a machine with no ROS installed. The vtable's
 * function slots are trampolines — the per-distro inner binary
 * (`dist/<distro>/libros2_stream_plugin-<distro>.pjros2` relative to this library)
 * is dlopen-ed lazily on the first slot call (i.e. when the source is actually
 * instantiated), never at discovery time. Once loaded, the trampolines forward
 * straight to the inner's vtable.
 *
 * This separation is deliberate: the proxy *is* the plugin (discoverable,
 * installable, manifest-bearing); the per-distro inner is an internal
 * implementation component pulled in on demand. If no ROS distro is present
 * the proxy still loads and lists cleanly, and instantiation fails with a
 * clear message (see PJ_get_proxy_last_error) instead of a dynamic-linker
 * error at install/scan time.
 *
 * The packaged inner DSO uses the private `.pjros2` suffix and deliberately
 * does NOT export the standard
 * `PJ_get_data_source_vtable` / `PJ_get_dialog_vtable` symbols — the
 * host's recursive plugin scanner (`scanPluginDsos`) would otherwise try to
 * inspect every per-distro inner as a top-level plugin. The private suffix
 * keeps these implementation libraries out of discovery, while the renamed
 * `PJ_ros2_inner_get_*_vtable` exports keep them reachable only through this
 * proxy.
 *
 * By design this translation unit has **no dependency** on rclcpp or any
 * ROS component. Only <dlfcn.h>, <filesystem> and <cstdlib>. This way
 * the proxy loads cleanly on machines without ROS installed.
 */

#include <dlfcn.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "pj_base/data_source_protocol.h"
#include "pj_base/plugin_abi_export.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_plugins/dialog_protocol.h"
#include "ros2_manifest.hpp"

namespace {

// Supported ROS 2 distributions, in preferred order. When the user has
// several installed and no explicit ROS_DISTRO env var, the proxy picks the
// highest-priority one present. LTS distros come first because they are the
// reasonable "default" for a user who hasn't sourced anything.
constexpr std::array<std::string_view, 4> kSupportedDistros = {"humble", "iron", "jazzy", "rolling"};
constexpr std::string_view kInnerLibrarySuffix = ".pjros2";

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
  if (auto env = PJ::sdk::getEnv("ROS_DISTRO")) {
    return env;
  }

  // 2. Standard apt/debian install: /opt/ros/<distro>/setup.bash
  for (const auto& distro : kSupportedDistros) {
    if (distroDirectoryExists(distro)) {
      return std::string(distro);
    }
  }

  // 3. RoboStack / conda overlay: $CONDA_PREFIX/share/<distro>
  if (auto prefix = PJ::sdk::getEnv("CONDA_PREFIX")) {
    const std::filesystem::path conda_prefix(*prefix);
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

// Resolves the distro, loads the inner library, captures its data-source and
// dialog vtables. On failure populates g_last_error and leaves the cached
// vtables null. Run exactly once via loadInnerOnce().
void loadInnerImpl() {
  auto distro = detectRosDistro();
  if (!distro.has_value()) {
    g_last_error = "ROS 2 not detected; source /opt/ros/<distro>/setup.bash before starting PlotJuggler.";
    return;
  }

  auto dir = proxyDirectory();
  if (dir.empty()) {
    g_last_error = "Unable to locate proxy shared library directory (dladdr failed).";
    return;
  }

  const auto inner_path =
      dir / "dist" / *distro / ("libros2_stream_plugin-" + *distro + std::string(kInnerLibrarySuffix));
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

// Runs loadInnerImpl exactly once and mirrors any failure reason to stderr. The
// host may surface g_last_error via the optional PJ_get_proxy_last_error symbol,
// but a host that doesn't know it would otherwise see a failed load as complete
// silence — so always log it once here.
void loadInnerOnce() {
  loadInnerImpl();
  if (g_data_source_vtable == nullptr && !g_last_error.empty()) {
    std::fprintf(stderr, "[ros2.proxy] %s\n", g_last_error.c_str());
  }
}

// Ensure the inner library is resolved (once), then hand back its data-source
// vtable. Returns null when no ROS distro / inner binary is available; the
// reason is in g_last_error (surfaced via PJ_get_proxy_last_error).
const PJ_data_source_vtable_t* innerDataSource() {
  std::call_once(g_load_once, loadInnerOnce);
  return g_data_source_vtable;
}

// Copy g_last_error (or a generic fallback) into a host-provided PJ_error_t.
// Used by the bool-returning trampolines when the inner cannot be loaded.
void fillProxyError(PJ_error_t* out_error) {
  if (out_error == nullptr) {
    return;
  }
  out_error->code = -1;
  std::snprintf(out_error->domain, sizeof(out_error->domain), "%s", "ros2.proxy");
  std::snprintf(
      out_error->message, sizeof(out_error->message), "%s",
      g_last_error.empty() ? "ROS 2 inner library unavailable." : g_last_error.c_str());
  out_error->extended = nullptr;
  out_error->extended_kind[0] = '\0';
}

// ---------------------------------------------------------------------------
// Trampolines. Each forwards verbatim to the inner vtable once it is resolved.
// Before the inner is available they degrade gracefully: create() returns null
// (so the host never reaches the lifecycle slots), and the fallible slots
// report g_last_error. The proxy holds no per-instance state of its own — the
// ctx pointer it returns from create() is the inner's, threaded straight back
// through every other slot.
// ---------------------------------------------------------------------------

void* proxyCreate() noexcept {
  const auto* inner = innerDataSource();
  if (inner == nullptr) {
    return nullptr;  // inner unavailable; g_last_error already set + logged by loadInnerOnce
  }
  void* ctx = inner->create();
  if (ctx == nullptr) {
    // The inner loaded but its create() failed (e.g. OOM). loadInnerOnce left no
    // reason, so capture one here rather than report a stale/empty message.
    g_last_error = "ROS 2 inner library loaded but failed to instantiate the data source.";
    std::fprintf(stderr, "[ros2.proxy] %s\n", g_last_error.c_str());
  }
  return ctx;
}

void proxyDestroy(void* ctx) noexcept {
  if (g_data_source_vtable != nullptr) {
    g_data_source_vtable->destroy(ctx);
  }
}

uint64_t proxyCapabilities(void* ctx) noexcept {
  return g_data_source_vtable != nullptr ? g_data_source_vtable->capabilities(ctx) : 0;
}

bool proxyBind(void* ctx, PJ_service_registry_t registry, PJ_error_t* out_error) noexcept {
  if (g_data_source_vtable == nullptr) {
    fillProxyError(out_error);
    return false;
  }
  return g_data_source_vtable->bind(ctx, registry, out_error);
}

bool proxySaveConfig(void* ctx, PJ_string_view_t* out_json, PJ_error_t* out_error) noexcept {
  if (g_data_source_vtable == nullptr) {
    fillProxyError(out_error);
    return false;
  }
  return g_data_source_vtable->save_config(ctx, out_json, out_error);
}

bool proxyLoadConfig(void* ctx, PJ_string_view_t config_json, PJ_error_t* out_error) noexcept {
  if (g_data_source_vtable == nullptr) {
    fillProxyError(out_error);
    return false;
  }
  return g_data_source_vtable->load_config(ctx, config_json, out_error);
}

bool proxyStart(void* ctx, PJ_error_t* out_error) noexcept {
  if (g_data_source_vtable == nullptr) {
    fillProxyError(out_error);
    return false;
  }
  return g_data_source_vtable->start(ctx, out_error);
}

void proxyStop(void* ctx) noexcept {
  if (g_data_source_vtable != nullptr) {
    g_data_source_vtable->stop(ctx);
  }
}

bool proxyPause(void* ctx, PJ_error_t* out_error) noexcept {
  if (g_data_source_vtable == nullptr) {
    fillProxyError(out_error);
    return false;
  }
  return g_data_source_vtable->pause(ctx, out_error);
}

bool proxyResume(void* ctx, PJ_error_t* out_error) noexcept {
  if (g_data_source_vtable == nullptr) {
    fillProxyError(out_error);
    return false;
  }
  return g_data_source_vtable->resume(ctx, out_error);
}

bool proxyPoll(void* ctx, PJ_error_t* out_error) noexcept {
  if (g_data_source_vtable == nullptr) {
    fillProxyError(out_error);
    return false;
  }
  return g_data_source_vtable->poll(ctx, out_error);
}

PJ_data_source_state_t proxyCurrentState(void* ctx) noexcept {
  // [thread-safe] slot: the host may call it from a thread other than the one
  // that ran create(). Go through innerDataSource() (std::call_once) so that
  // thread gets the acquire edge on the published vtable rather than reading the
  // global directly. Reachable only with a live ctx, so the load already ran.
  const auto* inner = innerDataSource();
  return inner != nullptr ? inner->current_state(ctx) : PJ_DATA_SOURCE_STATE_FAILED;
}

PJ_borrowed_dialog_t proxyGetDialog(void* ctx) noexcept {
  return g_data_source_vtable != nullptr ? g_data_source_vtable->get_dialog(ctx)
                                         : PJ_borrowed_dialog_t{nullptr, nullptr};
}

const void* proxyGetPluginExtension(void* ctx, PJ_string_view_t id) noexcept {
  // [thread-safe] slot (see proxyCurrentState): route through innerDataSource()
  // for the std::call_once acquire edge. Reachable only with a live ctx.
  const auto* inner = innerDataSource();
  return inner != nullptr ? inner->get_plugin_extension(ctx, id) : nullptr;
}

// Self-describing proxy vtable. Carries the extension's embedded manifest so
// the host scanner can discover/catalog the plugin with no ROS present; the
// inner binary is only dlopen-ed when a slot is first invoked.
constexpr PJ_data_source_vtable_t kProxyVtable = {
    /* protocol_version    */ PJ_DATA_SOURCE_PROTOCOL_VERSION,
    /* struct_size         */ sizeof(PJ_data_source_vtable_t),
    /* create              */ &proxyCreate,
    /* destroy             */ &proxyDestroy,
    /* manifest_json       */ kRos2ProxyManifest,
    /* capabilities        */ &proxyCapabilities,
    /* bind                */ &proxyBind,
    /* save_config         */ &proxySaveConfig,
    /* load_config         */ &proxyLoadConfig,
    /* start               */ &proxyStart,
    /* stop                */ &proxyStop,
    /* pause               */ &proxyPause,
    /* resume              */ &proxyResume,
    /* poll                */ &proxyPoll,
    /* current_state       */ &proxyCurrentState,
    /* get_dialog          */ &proxyGetDialog,
    /* get_plugin_extension*/ &proxyGetPluginExtension,
};

}  // namespace

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() {
  // No eager load: returning the static vtable lets the scanner read the
  // manifest without touching ROS. The inner is resolved lazily on first use.
  return &kProxyVtable;
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
