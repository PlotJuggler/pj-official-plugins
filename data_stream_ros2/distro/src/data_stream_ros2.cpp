/**
 * @file data_stream_ros2.cpp
 * @brief ROS 2 topic subscriber — per-distro implementation.
 *
 * Subscribes to a user-selected list of ROS 2 topics, collects raw CDR bytes
 * via `GenericSubscription`, and hands them off to the host parser registry
 * (delegated ingest). The actual decoding lives in `parser_ros`, which we
 * reach via `runtimeHost().ensureParserBinding({encoding="ros2msg", ...})`.
 *
 * Threading: a `MultiThreadedExecutor` runs in `spinner_`, callbacks enqueue
 * messages into a mutex-protected queue, and `onPoll()` drains the queue on
 * the host's polling thread (per the SDK contract — host write methods may
 * only be called from `onPoll()`).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <queue>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/generic_subscription.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialized_message.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ros2_dialog.hpp"
#include "ros2_manifest.hpp"
#include "ros2_qos_adapter.hpp"
#include "ros2_schema_builder.hpp"

namespace {

struct PendingMessage {
  std::string topic;
  // Hold the rclcpp::SerializedMessage shared_ptr so its rcl buffer survives
  // any deferred FetchMessageData pull the host issues. The same shared_ptr
  // is forwarded as the PayloadView anchor at push time.
  std::shared_ptr<rclcpp::SerializedMessage> msg;
  int64_t timestamp_ns;
};

class Ros2StreamSource : public PJ::StreamSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    // Forward unconditionally (even on an empty config): dialog_.loadConfig()
    // both restores any saved selection and starts topic discovery, which must
    // run on a fresh open too. Mirrors data_stream_mqtt's loadConfig.
    (void)dialog_.loadConfig(config_json);
    // Extract parser config (e.g. "use_embedded_timestamp") persisted by the
    // host under "_parser_config" so it reaches ensureParserBinding.
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded() && cfg.contains("_parser_config")) {
      parser_config_override_ = cfg["_parser_config"].get<std::string>();
    } else {
      parser_config_override_.clear();
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }
    selected_topics_.clear();
    if (cfg.contains("selected_topics") && cfg["selected_topics"].is_array()) {
      for (const auto& entry : cfg["selected_topics"]) {
        if (!entry.is_object()) {
          continue;
        }
        std::string name = entry.value("name", std::string{});
        std::string type = entry.value("type", std::string{});
        if (!name.empty() && !type.empty()) {
          selected_topics_.emplace_back(std::move(name), std::move(type));
        }
      }
    }
    if (selected_topics_.empty()) {
      return PJ::unexpected("no ROS 2 topics selected");
    }

    // Stash the parser_config sub-object as a string for every ensureBinding
    // call. parser_ros::loadConfig accepts unknown keys silently, so unknown
    // future options pass through harmlessly.
    parser_config_json_.clear();
    if (cfg.contains("parser_config") && cfg["parser_config"].is_object()) {
      parser_config_json_ = cfg["parser_config"].dump();
    }

    try {
      context_ = std::make_shared<rclcpp::Context>();
      context_->init(0, nullptr);

      rclcpp::NodeOptions node_opts;
      node_opts.context(context_);
      node_ = std::make_shared<rclcpp::Node>("plotjuggler_ros2_subscriber", node_opts);

      rclcpp::ExecutorOptions exec_opts;
      exec_opts.context = context_;
      executor_ = std::make_unique<rclcpp::executors::MultiThreadedExecutor>(exec_opts, 2);
      executor_->add_node(node_);

      for (const auto& [topic, type] : selected_topics_) {
        const auto qos = ros2_streamer::adaptQosWaitingForPublishers(*node_, topic);
        if (node_->count_publishers(topic) == 0) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "No publishers visible for " + topic + " within discovery timeout — using default QoS");
        }

        auto callback = [this, topic](std::shared_ptr<rclcpp::SerializedMessage> msg) {
          enqueueMessage(topic, std::move(msg));
        };

        auto subscription = node_->create_generic_subscription(topic, type, qos, callback);
        subscriptions_.emplace(topic, std::move(subscription));
      }

      running_ = true;
      spinner_ = std::thread([this] {
        while (running_) {
          executor_->spin_once(std::chrono::milliseconds(50));
        }
      });
    } catch (const std::exception& e) {
      teardown();
      return PJ::unexpected(std::string("ROS 2 setup failed: ") + e.what());
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    std::queue<PendingMessage> batch;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::swap(batch, message_queue_);
    }

    while (!batch.empty()) {
      auto& msg = batch.front();
      auto* binding = ensureBinding(msg.topic);
      if (binding != nullptr) {
        // Zero-copy push: the FetchMessageData closure captures the
        // SerializedMessage shared_ptr (which owns the rcl buffer) and hands
        // the host a PayloadView pointing at that buffer with the shared_ptr
        // as the BufferAnchor. The host applies ObjectIngestPolicy to decide
        // whether to invoke the closure now (eager), on consumer pull
        // (lazy), or never. The buffer survives any pending pull because
        // every captured shared_ptr keeps the SerializedMessage alive.
        auto status = runtimeHost().pushMessage(
            *binding, PJ::Timestamp{msg.timestamp_ns}, [keeper = msg.msg]() -> PJ::sdk::PayloadView {
              const auto& rcl = keeper->get_rcl_serialized_message();
              return PJ::sdk::PayloadView{
                  PJ::Span<const uint8_t>{rcl.buffer, rcl.buffer_length},
                  PJ::sdk::BufferAnchor{keeper},
              };
            });
        if (!status) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "Failed to push ROS 2 message on " + msg.topic + ": " + status.error());
        }
      }
      batch.pop();
    }
    return PJ::okStatus();
  }

  void onStop() override {
    teardown();
  }

 private:
  void enqueueMessage(const std::string& topic, std::shared_ptr<rclcpp::SerializedMessage> msg) {
    if (!msg) {
      return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    PendingMessage pending{
        .topic = topic,
        .msg = std::move(msg),
        .timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
    };

    std::lock_guard<std::mutex> lock(queue_mutex_);
    message_queue_.push(std::move(pending));
  }

  PJ::ParserBindingHandle* ensureBinding(const std::string& topic) {
    auto it = binding_cache_.find(topic);
    if (it != binding_cache_.end()) {
      return &it->second;
    }

    std::string type_name;
    for (const auto& [t, ty] : selected_topics_) {
      if (t == topic) {
        type_name = ty;
        break;
      }
    }
    if (type_name.empty()) {
      return nullptr;
    }

    std::string schema;
    try {
      schema = ros2_streamer::buildRos2Schema(type_name);
    } catch (const std::exception& e) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "Failed to build schema for " + topic + " (" + type_name + "): " + e.what());
      return nullptr;
    }

    nlohmann::json parser_config = nlohmann::json::object();
    const std::string& base_parser_config =
        !parser_config_override_.empty() ? parser_config_override_ : parser_config_json_;
    if (!base_parser_config.empty()) {
      auto parsed_config = nlohmann::json::parse(base_parser_config, nullptr, false);
      if (parsed_config.is_object()) {
        parser_config = std::move(parsed_config);
      }
    }
    parser_config["schema_encoding"] = "ros2msg";
    const std::string parser_config_str = parser_config.dump();

    auto binding = runtimeHost().ensureParserBinding({
        .topic_name = topic,
        .parser_encoding = "ros2msg",
        .type_name = type_name,
        .schema = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema.data()), schema.size()),
        .parser_config_json = parser_config_str,
    });
    if (!binding) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "Parser binding failed for " + topic + ": " + binding.error());
      return nullptr;
    }
    auto [iter, _] = binding_cache_.emplace(topic, *binding);
    return &iter->second;
  }

  void teardown() {
    running_ = false;
    if (executor_) {
      executor_->cancel();
    }
    if (spinner_.joinable()) {
      spinner_.join();
    }
    subscriptions_.clear();
    if (executor_ && node_) {
      executor_->remove_node(node_);
    }
    executor_.reset();
    node_.reset();
    if (context_) {
      try {
        context_->shutdown("plugin stopping");
      } catch (...) {}
      context_.reset();
    }
    binding_cache_.clear();
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::queue<PendingMessage> empty;
      std::swap(message_queue_, empty);
    }
  }

  Ros2Dialog dialog_;
  std::vector<std::pair<std::string, std::string>> selected_topics_;
  std::string parser_config_json_;

  std::shared_ptr<rclcpp::Context> context_;
  std::shared_ptr<rclcpp::Node> node_;
  std::unique_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  std::unordered_map<std::string, std::shared_ptr<rclcpp::GenericSubscription>> subscriptions_;
  std::thread spinner_;
  std::atomic<bool> running_{false};

  std::string parser_config_override_;
  std::mutex queue_mutex_;
  std::queue<PendingMessage> message_queue_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
};

}  // namespace

// The inner DSO is loaded only by the proxy via dlopen + dlsym on private
// symbols. We deliberately do NOT export PJ_get_data_source_vtable nor
// PJ_get_dialog_vtable — those are the names the host's plugin scanner
// looks for, and the scanner walks the extension directory recursively.
// If the inner exported the standard names, every per-distro inner would
// be picked up as a top-level plugin (one per ROS distro) instead of the
// single proxy entry point. Renamed exports keep the inner invisible to
// the scanner while remaining reachable from the proxy.
extern "C" __attribute__((visibility("default"))) const PJ_data_source_vtable_t*
PJ_ros2_inner_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t* vt = PJ::DataSourcePluginBase::vtableWithCreate(
      []() noexcept -> void* {
        try {
          return new Ros2StreamSource();
        } catch (...) {
          return nullptr;
        }
      },
      kRos2Manifest);
  return vt;
}

extern "C" __attribute__((visibility("default"))) const PJ_dialog_vtable_t* PJ_ros2_inner_get_dialog_vtable() noexcept {
  static const PJ_dialog_vtable_t* vt = PJ::DialogPluginBase::vtableWithCreate([]() noexcept -> void* {
    try {
      return new Ros2Dialog();
    } catch (...) {
      return nullptr;
    }
  });
  return vt;
}

// `Ros2StreamSource::getDialog()` calls PJ::borrowDialog(dialog_), which
// expands to PJ::dialogVtableFor<Ros2Dialog>(). The PJ_DIALOG_PLUGIN macro
// is what normally specialises that template, but we don't use the macro
// here (it would also export the standard symbol the host scanner picks
// up). Provide the specialisation explicitly, pointing at the private
// inner getter.
namespace PJ {
template <>
[[maybe_unused]] inline const PJ_dialog_vtable_t* dialogVtableFor<Ros2Dialog>() noexcept {
  return PJ_ros2_inner_get_dialog_vtable();
}
}  // namespace PJ
