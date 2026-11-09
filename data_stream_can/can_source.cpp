#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/dictionary_loader.hpp>
#include <pj_can_dbc/frame_router.hpp>
#include <string>
#include <string_view>

#include "bus_name.hpp"
#include "can_bus_reader.hpp"
#include "can_dialog.hpp"
#include "can_stream_manifest.hpp"

namespace {

/// Streams one CAN interface (QtSerialBus, on CanBusReader's thread) and routes every frame through the
/// shared pj_can_dbc router: decoded to "CAN/<bus>/<Message>", or raw to "CAN/<bus>/0x<ID>".
/// onStart runs on the host's GUI thread, onPoll / onStop on its session worker, never concurrently.
class CanSource : public PJ::StreamSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!config_json.empty() && !dialog_.loadConfig(config_json)) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    router_.reset();  // it points at the decoder: drop it first
    decoder_.emplace();
    const auto& dictionaries = dialog_.dictionaries();
    for (const auto& path : dictionaries) {
      std::string warnings;
      const PJ::Status loaded = pj_can_dbc::loadDictionary(*decoder_, path, warnings);
      if (!warnings.empty()) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, warnings);
      }
      if (!loaded) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, loaded.error());
      }
    }
    if (!dictionaries.empty() && decoder_->messageCount() == 0) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "no CAN message loaded from the dictionaries; nothing will be decoded");
    }

    const can_stream::BusSettings settings = dialog_.busSettings();
    bus_ = can_stream::busName(settings.interface_name);
    router_.emplace(dialog_.rawUnassigned());
    router_->setDecoder(bus_, dictionaries.empty() ? nullptr : &*decoder_);

    if (auto started = reader_.start(settings); !started) {
      router_.reset();  // no session: no summary on the stop that follows
      return started;
    }
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    if (!router_) {
      return PJ::okStatus();
    }
    routeFrames();
    if (auto error = reader_.takeFatalError()) {
      // Frames that arrived before the fatal error are still published. The reader thread pushes a
      // frame before it sets the error, so every frame it pushed before the error is in the queue by
      // the time take() sees it: one more drain catches those that landed after the drain above.
      routeFrames();
      // Returned only: the host turns a failed poll() into its own stream-error dialog, so a
      // reportMessage(kError) here would show the same error twice.
      return PJ::unexpected(std::move(*error));
    }
    return PJ::okStatus();
  }

  void onStop() override {
    reader_.stop();
    if (!router_) {
      return;  // never started, or already stopped
    }
    const pj_can_dbc::RouterCounts& routed = router_->counts();
    const can_stream::ReaderCounts read = reader_.counts();
    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "CAN " + bus_ + ": decoded " + std::to_string(routed.decoded) + " frame(s) into " +
            std::to_string(routed.decoded_topics) + " message topic(s); " + std::to_string(routed.unmatched) +
            " unmatched, " + std::to_string(routed.undecodable) + " undecodable, " +
            std::to_string(routed.no_dictionary) + " with no dictionary; " + std::to_string(read.error_frames) +
            " error frame(s), " + std::to_string(read.remote_frames) + " remote frame(s)");
    router_.reset();
  }

 private:
  void routeFrames() {
    for (auto frames = reader_.drainFrames(); !frames.empty(); frames.pop()) {
      const auto& frame = frames.front();
      router_->route(writeHost(), bus_, frame.can_id, frame.extended, frame.data, frame.ts_ns);
    }
  }

  can_stream::CanDialog dialog_;
  can_stream::CanBusReader reader_;
  // Declared before the router, which points at it: destroyed after it.
  std::optional<pj_can_dbc::CanDecoder> decoder_;
  // Pinned in place (it caches a pointer into itself): emplaced in onStart, never assigned.
  std::optional<pj_can_dbc::CanFrameRouter> router_;
  std::string bus_;
};

}  // namespace

PJ_DIALOG_PLUGIN(can_stream::CanDialog, kCanStreamManifest)
PJ_DATA_SOURCE_PLUGIN(CanSource, kCanStreamManifest)
