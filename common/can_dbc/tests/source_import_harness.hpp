#pragma once

// Shared by the layout-path tests of data_load_blf / data_load_candump /
// data_load_mf4: drives a BUILT data-source plugin the way the host does
// (bind -> loadConfig -> start -> saveConfig -> stop) against an in-memory
// write host and runtime host, and returns what the plugin wrote, said and
// saved. Header-only so each plugin's test target stays self-contained.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_can_dbc/dictionary_paths.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "dictionary_test_files.hpp"

namespace pj_can_dbc::testing {

namespace fs = std::filesystem;

/// What one import produced.
struct ImportCapture {
  struct Message {
    int level = 0;
    std::string text;
  };

  bool ok = false;
  std::string error;                          ///< bind/loadConfig/start failure
  std::string saved_config;                   ///< saveConfig() after the import
  std::map<std::string, std::uint64_t> rows;  ///< topic name -> records appended
  std::vector<Message> messages;              ///< report_message calls, in order

  bool hasTopic(const std::string& name) const {
    return rows.find(name) != rows.end();
  }

  bool hasMessage(PJ_data_source_message_level_t level, std::string_view substring) const {
    return std::any_of(messages.begin(), messages.end(), [&](const Message& message) {
      return message.level == static_cast<int>(level) && message.text.find(substring) != std::string::npos;
    });
  }
};

namespace detail {

class CaptureHost {
 public:
  std::map<std::uint32_t, std::string> topic_names;
  std::map<std::string, std::uint64_t> rows;
  std::vector<ImportCapture::Message> messages;

  PJ_source_write_host_t writeHost() {
    static const PJ_source_write_host_vtable_t vtable = [] {
      PJ_source_write_host_vtable_t v{};
      v.abi_version = PJ_PLUGIN_DATA_API_VERSION;
      v.struct_size = sizeof(v);
      v.ensure_topic = &CaptureHost::ensureTopic;
      v.ensure_field = &CaptureHost::ensureField;
      v.append_record = &CaptureHost::appendRecord;
      v.append_bound_record = &CaptureHost::appendBoundRecord;
      v.append_arrow_stream = &CaptureHost::appendArrowStream;
      return v;
    }();
    return PJ_source_write_host_t{this, &vtable};
  }

  PJ_data_source_runtime_host_t runtimeHost() {
    static const PJ_data_source_runtime_host_vtable_t vtable = [] {
      PJ_data_source_runtime_host_vtable_t v{};
      v.protocol_version = 1;
      v.struct_size = sizeof(v);
      v.report_message = &CaptureHost::reportMessage;
      v.progress_start = &CaptureHost::progressStart;
      v.progress_update = &CaptureHost::progressUpdate;
      v.progress_finish = &CaptureHost::progressFinish;
      v.is_stop_requested = &CaptureHost::isStopRequested;
      v.notify_state = &CaptureHost::notifyState;
      v.request_stop = &CaptureHost::requestStop;
      v.show_message_box = &CaptureHost::showMessageBox;
      v.list_available_encodings = &CaptureHost::listEncodings;
      return v;  // tail slots stay null
    }();
    return PJ_data_source_runtime_host_t{this, &vtable};
  }

 private:
  static std::string toString(PJ_string_view_t view) {
    return view.data == nullptr ? std::string{} : std::string(view.data, view.size);
  }

  void count(PJ_topic_handle_t topic) {
    const auto it = topic_names.find(topic.id);
    ++rows[it == topic_names.end() ? std::string() : it->second];
  }

  static bool ensureTopic(void* ctx, PJ_string_view_t name, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
    auto* self = static_cast<CaptureHost*>(ctx);
    for (const auto& [id, known] : self->topic_names) {
      if (known == toString(name)) {
        *out = PJ_topic_handle_t{id};
        return true;
      }
    }
    const auto id = static_cast<std::uint32_t>(self->topic_names.size() + 1);
    self->topic_names[id] = toString(name);
    *out = PJ_topic_handle_t{id};
    return true;
  }
  static bool ensureField(
      void*, PJ_topic_handle_t topic, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out,
      PJ_error_t*) noexcept {
    *out = PJ_field_handle_t{topic, 0};
    return true;
  }
  static bool appendRecord(
      void* ctx, PJ_topic_handle_t topic, int64_t, const PJ_named_field_value_t*, uint64_t, PJ_error_t*) noexcept {
    static_cast<CaptureHost*>(ctx)->count(topic);
    return true;
  }
  static bool appendBoundRecord(
      void* ctx, PJ_topic_handle_t topic, int64_t, const PJ_bound_field_value_t*, uint64_t, PJ_error_t*) noexcept {
    static_cast<CaptureHost*>(ctx)->count(topic);
    return true;
  }
  static bool appendArrowStream(
      void*, PJ_topic_handle_t, struct ArrowArrayStream*, PJ_string_view_t, PJ_error_t*) noexcept {
    return false;
  }

  static void reportMessage(void* ctx, PJ_data_source_message_level_t level, PJ_string_view_t text) noexcept {
    static_cast<CaptureHost*>(ctx)->messages.push_back({static_cast<int>(level), toString(text)});
  }
  static bool progressStart(void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept {
    return true;
  }
  static bool progressUpdate(void*, uint64_t) noexcept {
    return true;  // a null slot would mean "cancelled"
  }
  static void progressFinish(void*) noexcept {}
  static bool isStopRequested(void*) noexcept {
    return false;
  }
  static void notifyState(void*, PJ_data_source_state_t) noexcept {}
  static void requestStop(void*, PJ_data_source_state_t, PJ_string_view_t) noexcept {}
  static int showMessageBox(void*, PJ_message_box_type_t, PJ_string_view_t, PJ_string_view_t, int) noexcept {
    return -1;
  }
  static const char* listEncodings(void*) noexcept {
    return "[]";
  }
};

}  // namespace detail

/// Load `plugin_path`, hand it `config_json` as the host would, import, and return what happened.
inline ImportCapture runImport(const std::string& plugin_path, const std::string& config_json) {
  ImportCapture result;
  // Declared before the handle: the handle's destructor calls into the library.
  auto library = PJ::DataSourceLibrary::load(plugin_path);
  if (!library) {
    result.error = "load: " + library.error();
    return result;
  }
  detail::CaptureHost host;
  {
    auto handle = library->createHandle();
    PJ::ServiceRegistryBuilder registry;
    registry.registerService<PJ::sdk::SourceWriteHostService>(host.writeHost());
    registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(host.runtimeHost());
    if (auto status = handle.bind(registry.view()); !status) {
      result.error = "bind: " + status.error();
    } else if (auto loaded = handle.loadConfig(config_json); !loaded) {
      result.error = "loadConfig: " + loaded.error();
    } else {
      auto started = handle.start();
      result.ok = static_cast<bool>(started);
      if (!started) {
        result.error = started.error();
      }
      if (auto saved = handle.saveConfig(result.saved_config); !saved) {
        result.saved_config = "saveConfig failed: " + saved.error();
      }
      handle.stop();
    }
  }
  result.rows = host.rows;
  result.messages = host.messages;
  return result;
}

/// What a plugin supplies to the shared layout-path scenarios.
struct LayoutPathsSpec {
  std::string plugin_path;   ///< the built data-source plugin
  std::string data_name;     ///< data file name inside the layout folder, e.g. "run.blf"
  std::string topic_prefix;  ///< topics decoded from vehicle.dbc are topic_prefix + <message name>
  int dlc = 8;               ///< DLC of the test DBC message, matching the data fixture's frames
  /// Writes the data file at the given path.
  std::function<void(const fs::path& data)> writeData;
  /// The plugin's config: "filepath" = data, the dictionary list (one JSON array of strings) in its config shape,
  /// plus the members of `extra`.
  std::function<std::string(const fs::path& data, const nlohmann::json& dict_entries, const nlohmann::json& extra)>
      makeConfig;
  /// The saved dictionary list of that same plugin, read out of its saveConfig() JSON.
  std::function<nlohmann::json(const nlohmann::json& saved)> dictsOf;
};

/// Fixture and scenarios shared by the three plugins' layout-path tests. A layout folder `old/` holds the data
/// file and vehicle.dbc; each scenario moves or copies it and checks which DBC decodes.
class LayoutPathsFixture : public ::testing::Test {
 protected:
  virtual const LayoutPathsSpec& spec() const = 0;

  void SetUp() override {
    old_ = scratch_.root() / "old";
    fs::create_directories(old_);
    spec().writeData(old_ / spec().data_name);
    writeEngineDbc(old_ / "vehicle.dbc", "Engine", spec().dlc);
  }

  using json = nlohmann::json;

  std::string config(const fs::path& data, const json& dict_entries, const json& extra = json::object()) const {
    return spec().makeConfig(data, dict_entries, extra);
  }
  ImportCapture runWith(const fs::path& data, const json& dict_entries, const json& extra = json::object()) const {
    return runImport(spec().plugin_path, config(data, dict_entries, extra));
  }
  std::string topic(const std::string& message) const {
    return spec().topic_prefix + message;
  }

  /// Saving beside the data stores the DBC relative; an external one stays absolute; the host-private key is
  /// dropped.
  template <class Dialog>
  void checkSaveStoresRelative() {
    Dialog dialog;
    const fs::path external = scratch_.root() / "shared" / "ext.dbc";
    ASSERT_TRUE(dialog.loadConfig(config(
        old_ / spec().data_name, json::array({generic(old_ / "vehicle.dbc"), generic(external)}),
        {{pj_can_dbc::kSavedFilepathKey, "stale"}})));

    const json saved = json::parse(dialog.saveConfig());
    EXPECT_EQ(spec().dictsOf(saved), json::array({"vehicle.dbc", generic(external)}));
    EXPECT_EQ(saved[pj_can_dbc::kAuxBaseDirKey], generic(old_));
    EXPECT_FALSE(saved.contains(pj_can_dbc::kSavedFilepathKey));
  }

  void checkMovedLayoutDecodesBesideNewData() {
    const fs::path moved = scratch_.root() / "new";
    fs::rename(old_, moved);

    const auto result = runWith(
        moved / spec().data_name, json::array({"vehicle.dbc"}),
        {{pj_can_dbc::kAuxBaseDirKey, generic(old_)},
         {pj_can_dbc::kSavedFilepathKey, generic(old_ / spec().data_name)}});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.hasTopic(topic("Engine")));
    EXPECT_FALSE(result.hasMessage(PJ_DATA_SOURCE_MESSAGE_WARNING, "not found"));
    const json saved = json::parse(result.saved_config);
    EXPECT_FALSE(saved.contains(pj_can_dbc::kSavedFilepathKey));
    EXPECT_EQ(spec().dictsOf(saved), json::array({"vehicle.dbc"}));
    EXPECT_EQ(saved[pj_can_dbc::kAuxBaseDirKey], generic(moved));
  }

  /// Where a scenario's files sit inside a layout folder.
  struct Placement {
    fs::path data;  ///< the data file
    fs::path dbc;   ///< vehicle.dbc
  };

  /// Lays out old/ with the data file under `data_sub` and vehicle.dbc under `dbc_sub` (both relative to old/;
  /// empty = directly in old/, the SetUp layout). Real layouts often keep them in sibling folders (data/, dbc/).
  Placement layOutOld(const fs::path& data_sub, const fs::path& dbc_sub, const std::string& message) {
    if (!data_sub.empty() || !dbc_sub.empty()) {
      fs::remove_all(old_);
      fs::create_directories(old_ / data_sub);
      spec().writeData(old_ / data_sub / spec().data_name);
    }
    writeEngineDbc(old_ / dbc_sub / "vehicle.dbc", message, spec().dlc);
    return {data_sub / spec().data_name, dbc_sub / "vehicle.dbc"};
  }

  void checkMovedLegacyAbsoluteConfigIsRebased(const fs::path& data_sub = {}, const fs::path& dbc_sub = {}) {
    const auto place = layOutOld(data_sub, dbc_sub, "Engine");
    const fs::path moved = scratch_.root() / "new";
    const std::string old_dbc = generic(old_ / place.dbc);
    const std::string old_data = generic(old_ / place.data);
    fs::rename(old_, moved);

    const auto result =
        runWith(moved / place.data, json::array({old_dbc}), {{pj_can_dbc::kSavedFilepathKey, old_data}});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.hasTopic(topic("Engine")));
    EXPECT_FALSE(result.hasMessage(PJ_DATA_SOURCE_MESSAGE_WARNING, "not found"));
  }

  void checkCopiedLayoutUsesTheCopysDbc(const fs::path& data_sub = {}, const fs::path& dbc_sub = {}) {
    const auto place = layOutOld(data_sub, dbc_sub, "OldEngine");
    const fs::path copy = scratch_.root() / "new";
    fs::copy(old_, copy, fs::copy_options::recursive);
    writeEngineDbc(copy / place.dbc, "NewEngine", spec().dlc);

    const std::string old_data = generic(old_ / place.data);
    const auto legacy = runWith(
        copy / place.data, json::array({generic(old_ / place.dbc)}), {{pj_can_dbc::kSavedFilepathKey, old_data}});
    ASSERT_TRUE(legacy.ok) << legacy.error;
    EXPECT_TRUE(legacy.hasTopic(topic("NewEngine")));
    EXPECT_FALSE(legacy.hasTopic(topic("OldEngine")));

    // Relative entries are relative to the data file's directory, so only the flat layout has one to test.
    if (dbc_sub != data_sub) {
      return;
    }
    const auto relative = runWith(
        copy / place.data, json::array({"vehicle.dbc"}),
        {{pj_can_dbc::kAuxBaseDirKey, generic(old_ / data_sub)}, {pj_can_dbc::kSavedFilepathKey, old_data}});
    ASSERT_TRUE(relative.ok) << relative.error;
    EXPECT_TRUE(relative.hasTopic(topic("NewEngine")));
    EXPECT_FALSE(relative.hasTopic(topic("OldEngine")));
  }

  void checkMissingDbcReportsNotFoundWarning() {
    const auto result = runWith(old_ / spec().data_name, json::array({"gone.dbc"}));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.hasMessage(PJ_DATA_SOURCE_MESSAGE_WARNING, "DBC file not found: " + generic(old_ / "gone.dbc")));
    EXPECT_FALSE(result.hasMessage(PJ_DATA_SOURCE_MESSAGE_WARNING, "version header"));
  }

  /// The host pre-fills an interactive open with the last accepted config but only overwrites "filepath": the
  /// saved base directory keeps relative entries valid. `loaded_paths(dialog)` returns the dialog's in-memory
  /// dictionary list of the config's entry.
  template <class Dialog, class LoadedPaths>
  void checkPrefillForAnotherDataFileKeepsSavedBase(LoadedPaths loaded_paths) {
    const fs::path other = scratch_.root() / "other" / ("other_" + spec().data_name);
    Dialog dialog;
    ASSERT_TRUE(
        dialog.loadConfig(config(other, json::array({"vehicle.dbc"}), {{pj_can_dbc::kAuxBaseDirKey, generic(old_)}})));

    EXPECT_EQ(loaded_paths(dialog), std::vector<std::string>({generic(old_ / "vehicle.dbc")}));

    const json saved = json::parse(dialog.saveConfig());
    EXPECT_EQ(spec().dictsOf(saved), json::array({generic(old_ / "vehicle.dbc")}));
    EXPECT_EQ(saved[pj_can_dbc::kAuxBaseDirKey], generic(other.parent_path()));
  }

  ScratchDir scratch_;
  fs::path old_;
};
}  // namespace pj_can_dbc::testing
