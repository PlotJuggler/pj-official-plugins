#include "ros2_trace_model/registry.hpp"

namespace ros2_trace_model {

void Registry::consume(const RawEvent& ev) {
  switch (ev.tp()) {
    case Tp::RclNodeInit: {
      const auto handle = ev.handle("node_handle");
      if (!handle) {
        return;
      }
      NodeInfo info;
      info.name = std::string(ev.str("node_name").value_or(std::string_view{}));
      info.ns = std::string(ev.str("namespace").value_or(std::string_view{}));
      nodes_[EntityKey{*handle}] = std::move(info);
      break;
    }
    case Tp::RclSubscriptionInit: {
      const auto sub_handle = ev.handle("subscription_handle");
      const auto node_handle = ev.handle("node_handle");
      if (!sub_handle || !node_handle) {
        return;
      }
      SubInfo info;
      info.topic = std::string(ev.str("topic_name").value_or(std::string_view{}));
      info.node = EntityKey{*node_handle};
      subscriptions_[EntityKey{*sub_handle}] = std::move(info);
      if (const auto rmw = ev.handle("rmw_subscription_handle")) {
        rmw_subs_[EntityKey{*rmw}] = EntityKey{*sub_handle};
      }
      break;
    }
    case Tp::RclcppSubscriptionInit: {
      const auto sub_handle = ev.handle("subscription_handle");
      const auto sub_obj = ev.handle("subscription");
      if (!sub_handle || !sub_obj) {
        return;
      }
      sub_objects_[EntityKey{*sub_obj}] = EntityKey{*sub_handle};
      break;
    }
    case Tp::RclcppSubscriptionCallbackAdded: {
      const auto sub_obj = ev.handle("subscription");
      const auto callback = ev.handle("callback");
      if (!sub_obj || !callback) {
        return;
      }
      callbacks_[EntityKey{*callback}] = CallbackOwner{CallbackKind::Subscription, EntityKey{*sub_obj}};
      break;
    }
    case Tp::RclcppCallbackRegister: {
      const auto callback = ev.handle("callback");
      if (!callback) {
        return;
      }
      callback_symbols_[EntityKey{*callback}] = std::string(ev.str("symbol").value_or(std::string_view{}));
      break;
    }
    case Tp::RclTimerInit: {
      const auto timer = ev.handle("timer_handle");
      if (!timer) {
        return;
      }
      timers_[EntityKey{*timer}].period_ns = ev.i64("period").value_or(0);
      break;
    }
    case Tp::RclcppTimerLinkNode: {
      const auto timer = ev.handle("timer_handle");
      const auto node_handle = ev.handle("node_handle");
      if (!timer || !node_handle) {
        return;
      }
      timers_[EntityKey{*timer}].node = EntityKey{*node_handle};
      break;
    }
    case Tp::RclcppTimerCallbackAdded: {
      const auto timer = ev.handle("timer_handle");
      const auto callback = ev.handle("callback");
      if (!timer || !callback) {
        return;
      }
      callbacks_[EntityKey{*callback}] = CallbackOwner{CallbackKind::Timer, EntityKey{*timer}};
      break;
    }
    default:
      break;
  }
}

std::optional<NodeInfo> Registry::node(EntityKey key) const {
  const auto it = nodes_.find(key);
  if (it == nodes_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<ResolvedCallback> Registry::resolveCallback(EntityKey callback) const {
  const auto cb_it = callbacks_.find(callback);
  if (cb_it == callbacks_.end()) {
    return std::nullopt;
  }

  ResolvedCallback out;
  out.kind = cb_it->second.kind;

  if (const auto sym_it = callback_symbols_.find(callback); sym_it != callback_symbols_.end()) {
    out.symbol = sym_it->second;
  }

  switch (cb_it->second.kind) {
    case CallbackKind::Subscription: {
      const auto so_it = sub_objects_.find(cb_it->second.owner);
      if (so_it == sub_objects_.end()) {
        break;
      }
      const auto sub_it = subscriptions_.find(so_it->second);
      if (sub_it == subscriptions_.end()) {
        break;
      }
      out.topic_name = sub_it->second.topic;
      if (const auto node_it = nodes_.find(sub_it->second.node); node_it != nodes_.end()) {
        out.node_name = node_it->second.name;
      }
      break;
    }
    case CallbackKind::Timer: {
      const auto t_it = timers_.find(cb_it->second.owner);
      if (t_it != timers_.end()) {
        if (const auto node_it = nodes_.find(t_it->second.node); node_it != nodes_.end()) {
          out.node_name = node_it->second.name;
        }
      }
      break;
    }
    case CallbackKind::Service:
    case CallbackKind::Unknown:
      break;
  }

  return out;
}

std::optional<std::string> Registry::topicForRmwSubscription(EntityKey rmw_handle) const {
  const auto it = rmw_subs_.find(rmw_handle);
  if (it == rmw_subs_.end()) {
    return std::nullopt;
  }
  const auto sub_it = subscriptions_.find(it->second);
  if (sub_it == subscriptions_.end()) {
    return std::nullopt;
  }
  return sub_it->second.topic;
}

}  // namespace ros2_trace_model
