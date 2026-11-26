# Header-only fake hosts and helpers shared by the plugin unit tests (namespace toolbox_testing):
# a catalog-only toolbox host, playback and viewport hosts, a recording data-processors host,
# scoped_env.hpp (environment-variable and temp-directory helpers), bind_scoped_registry.hpp (a
# service registry valid only during bind()) and widget_table.hpp (reads a dialog's table back
# from its widget data). Include as "test_support/<name>.hpp".
add_library(pj_test_support INTERFACE)
target_include_directories(pj_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pj_test_support INTERFACE plotjuggler_sdk::plugin_sdk)
