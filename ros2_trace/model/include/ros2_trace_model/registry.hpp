#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "ros2_trace_model/entity_key.hpp"
#include "ros2_trace_model/raw_event.hpp"

namespace ros2_trace_model {

struct NodeInfo {
  std::string name;
  std::string ns;
};

enum class CallbackKind {
  Unknown,
  Subscription,
  Timer,
  Service,
};

// A runtime callback handle resolved back to the human-readable entity that
// owns it. `topic_name` carries the topic for subscriptions, the service name
// for services, and is empty for timers.
struct ResolvedCallback {
  CallbackKind kind{CallbackKind::Unknown};
  std::string node_name;
  std::string topic_name;
  std::string symbol;
};

// Builds handle -> metadata tables from the trace's one-time init events, so
// that opaque runtime handle pointers can later be resolved to human-readable
// entities. Fed one RawEvent at a time, in timestamp order.
class Registry {
 public:
  void consume(const RawEvent& ev);

  std::optional<NodeInfo> node(EntityKey key) const;

  // Resolve a `callback` handle (as seen in callback_start/callback_end) to its
  // owning entity by walking the init-event chains.
  std::optional<ResolvedCallback> resolveCallback(EntityKey callback) const;

  // Resolve an rmw subscription handle (as seen in rmw_take) to its topic name.
  std::optional<std::string> topicForRmwSubscription(EntityKey rmw_handle) const;

 private:
  struct SubInfo {
    std::string topic;
    EntityKey node;
  };
  struct CallbackOwner {
    CallbackKind kind{CallbackKind::Unknown};
    EntityKey owner;  // subscription object / timer handle / service handle
  };
  struct TimerInfo {
    EntityKey node;
    std::int64_t period_ns{};
  };

  std::unordered_map<EntityKey, NodeInfo, EntityKeyHash> nodes_;
  std::unordered_map<EntityKey, SubInfo, EntityKeyHash> subscriptions_;  // by rcl subscription_handle
  std::unordered_map<EntityKey, EntityKey, EntityKeyHash> sub_objects_;  // rclcpp subscription -> subscription_handle
  std::unordered_map<EntityKey, EntityKey, EntityKeyHash> rmw_subs_;  // rmw subscription_handle -> subscription_handle
  std::unordered_map<EntityKey, TimerInfo, EntityKeyHash> timers_;    // by rcl timer_handle
  std::unordered_map<EntityKey, CallbackOwner, EntityKeyHash> callbacks_;  // callback -> owner
  std::unordered_map<EntityKey, std::string, EntityKeyHash> callback_symbols_;
};

}  // namespace ros2_trace_model
