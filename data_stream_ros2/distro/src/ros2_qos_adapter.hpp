#pragma once

/// @file ros2_qos_adapter.hpp
/// @brief Pick a QoS profile that is compatible with the publishers currently
/// advertising a topic.
///
/// Without this, sensor streams (BEST_EFFORT) and latched maps
/// (TRANSIENT_LOCAL) silently fail to deliver messages — the subscription is
/// created but no data flows because the requested QoS is incompatible with
/// the publisher's offer. The logic below mirrors `rosbag2_transport`'s
/// `Recorder::serialized_offered_qos_profiles_for_topic` strategy.

#include <rclcpp/node_interfaces/node_graph_interface.hpp>
#include <rclcpp/qos.hpp>
#include <rmw/qos_profiles.h>

#include <vector>

namespace ros2_streamer {

inline rclcpp::QoS adaptQosFromOffers(
    const std::vector<rclcpp::TopicEndpointInfo>& endpoints) {
  rclcpp::QoS qos(rmw_qos_profile_default.depth);
  if (endpoints.empty()) {
    return qos;
  }

  const size_t num_endpoints = endpoints.size();
  size_t reliable = 0;
  size_t transient_local = 0;
  for (const auto& ep : endpoints) {
    const auto& profile = ep.qos_profile().get_rmw_qos_profile();
    if (profile.reliability == RMW_QOS_POLICY_RELIABILITY_RELIABLE) {
      ++reliable;
    }
    if (profile.durability == RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL) {
      ++transient_local;
    }
  }

  if (reliable == num_endpoints) {
    qos.reliable();
  } else {
    qos.best_effort();
  }
  if (transient_local == num_endpoints) {
    qos.transient_local();
  } else {
    qos.durability_volatile();
  }
  return qos;
}

}  // namespace ros2_streamer
