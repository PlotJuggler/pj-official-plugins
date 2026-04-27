/**
 * @file data_stream_ros2.cpp
 * @brief ROS 2 topic subscriber plugin — build scaffolding.
 *
 * This is the initial skeleton: it establishes the shared-library entry
 * point, exercises the rclcpp linkage, and exports the minimum vtable
 * required so the host plugin loader recognises the library as a valid
 * PJ4 data-source plugin. The real subscriber logic (topic discovery,
 * QoS negotiation, per-topic threading, message decoding delegated to
 * parser_ros) will land in follow-up commits once the CI matrix proves
 * the build is clean across supported ROS distributions.
 */

#include <rclcpp/rclcpp.hpp>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/data_source_patterns.hpp"

#include "ros2_manifest.hpp"

namespace {

/// Minimal stub of a ROS 2 streaming data source.
///
/// Inherits from `StreamSourceBase` to match the streaming plugin pattern
/// used elsewhere (`data_stream_udp`, `data_stream_mqtt`, ...). Every
/// override is intentionally a no-op while the build scaffolding is being
/// validated. The goal of this first commit is to confirm that the plugin
/// compiles and links against every target ROS distribution via the CI
/// matrix. The real subscriber lands in follow-ups.
class Ros2StreamSource : public PJ::StreamSourceBase {
 public:
  uint64_t extraCapabilities() const override { return 0; }

  PJ::Status onStart() override {
    // Touch rclcpp so the linker actually pulls the symbol — prevents a
    // future "headers found but no real dependency on rclcpp" regression
    // from slipping through CI silently.
    (void)rclcpp::ok();
    return PJ::okStatus();
  }

  PJ::Status onPoll() override { return PJ::okStatus(); }

  void onStop() override {}
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(Ros2StreamSource, kRos2Manifest)
