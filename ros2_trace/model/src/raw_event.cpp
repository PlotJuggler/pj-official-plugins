#include "ros2_trace_model/raw_event.hpp"

#include <unordered_map>

namespace ros2_trace_model {

Tp classifyTracepoint(std::string_view name) {
  // CTF event-class names from LTTng-UST are "<provider>:<tracepoint>"; ros2's
  // provider is "ros2". Accept names with or without the prefix.
  constexpr std::string_view prefix = "ros2:";
  if (name.substr(0, prefix.size()) == prefix) {
    name.remove_prefix(prefix.size());
  }

  static const std::unordered_map<std::string_view, Tp> kMap = {
      {"rcl_init", Tp::RclInit},
      {"rcl_node_init", Tp::RclNodeInit},
      {"rmw_publisher_init", Tp::RmwPublisherInit},
      {"rcl_publisher_init", Tp::RclPublisherInit},
      {"rmw_subscription_init", Tp::RmwSubscriptionInit},
      {"rcl_subscription_init", Tp::RclSubscriptionInit},
      {"rclcpp_subscription_init", Tp::RclcppSubscriptionInit},
      {"rclcpp_subscription_callback_added", Tp::RclcppSubscriptionCallbackAdded},
      {"rcl_timer_init", Tp::RclTimerInit},
      {"rclcpp_timer_callback_added", Tp::RclcppTimerCallbackAdded},
      {"rclcpp_timer_link_node", Tp::RclcppTimerLinkNode},
      {"rcl_service_init", Tp::RclServiceInit},
      {"rclcpp_service_callback_added", Tp::RclcppServiceCallbackAdded},
      {"rcl_client_init", Tp::RclClientInit},
      {"rmw_client_init", Tp::RmwClientInit},
      {"rclcpp_callback_register", Tp::RclcppCallbackRegister},
      {"rcl_lifecycle_state_machine_init", Tp::RclLifecycleStateMachineInit},
      {"rcl_publish", Tp::RclPublish},
      {"rclcpp_publish", Tp::RclcppPublish},
      {"rmw_publish", Tp::RmwPublish},
      {"rclcpp_intra_publish", Tp::RclcppIntraPublish},
      {"rmw_take", Tp::RmwTake},
      {"rcl_take", Tp::RclTake},
      {"rclcpp_take", Tp::RclcppTake},
      {"callback_start", Tp::CallbackStart},
      {"callback_end", Tp::CallbackEnd},
      {"rclcpp_executor_wait_for_work", Tp::RclcppExecutorWaitForWork},
      {"rclcpp_executor_get_next_ready", Tp::RclcppExecutorGetNextReady},
      {"rclcpp_executor_execute", Tp::RclcppExecutorExecute},
      {"rcl_lifecycle_transition", Tp::RclLifecycleTransition},
  };

  const auto it = kMap.find(name);
  return it == kMap.end() ? Tp::Other : it->second;
}

const FieldValue* RawEvent::find(std::string_view name) const {
  for (const auto& f : fields_) {
    if (f.name == name) {
      return &f.value;
    }
  }
  return nullptr;
}

std::optional<std::uint64_t> RawEvent::handle(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<std::uint64_t>(v)) {
      return *p;
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> RawEvent::i64(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<std::int64_t>(v)) {
      return *p;
    }
  }
  return std::nullopt;
}

std::optional<bool> RawEvent::boolean(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<bool>(v)) {
      return *p;
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> RawEvent::str(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<std::string>(v)) {
      return std::string_view(*p);
    }
  }
  return std::nullopt;
}

}  // namespace ros2_trace_model
