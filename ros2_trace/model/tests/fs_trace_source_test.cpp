#include "ros2_trace_model/fs_trace_source.hpp"

#include <gtest/gtest.h>

using namespace ros2_trace_model;

// FsTraceSource only exists when the library was built with libbabeltrace2.
#ifdef ROS2_TRACE_WITH_BABELTRACE

// A path with no CTF metadata must fail cleanly (no events, no crash) rather
// than feed bad input to babeltrace2.
TEST(FsTraceSource, ReportsErrorForMissingMetadata) {
  FsTraceSource src("/nonexistent/ros2_trace/path");
  EXPECT_FALSE(src.ok());
  EXPECT_FALSE(src.next().has_value());
  EXPECT_EQ(src.size(), 0u);
}

#endif  // ROS2_TRACE_WITH_BABELTRACE
