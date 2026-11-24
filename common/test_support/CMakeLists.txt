# Header-only fake hosts shared by the plugin unit tests (namespace toolbox_testing): a catalog-only
# toolbox host, playback and viewport hosts and a recording data-processors host; scoped_env.hpp
# holds the environment-variable and temp-directory helpers, bind_scoped_registry.hpp a service
# registry valid only during bind(), widget_table.hpp reads a dialog's table back from its widget
# data (namespace toolbox_testing as well).
# Include them as "test_support/<name>.hpp". Internal target: not exported, not installed.
add_library(pj_test_support INTERFACE)
target_include_directories(pj_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pj_test_support INTERFACE plotjuggler_sdk::plugin_sdk)
