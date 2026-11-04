// SPDX-License-Identifier: MIT

#ifdef PJ_TEST_ROS2_DEPENDENCY
extern "C" int ros2_proxy_test_dependency();
#endif

// A real ELF dependency keeps dlopen failures independent of installed ROS.
extern "C" int ros2_proxy_test_anchor() {
#ifdef PJ_TEST_ROS2_DEPENDENCY
  return ros2_proxy_test_dependency();
#else
  return 0;
#endif
}
