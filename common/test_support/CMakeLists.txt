# Header-only fake hosts shared by the plugin unit tests (namespace toolbox_testing): a catalog-only
# toolbox host, a plot-tabs host, playback and viewport hosts and a recording data-processors host.
# Include them as "test_support/<name>.hpp". Internal target: not exported, not installed.
add_library(pj_test_support INTERFACE)
target_include_directories(pj_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pj_test_support INTERFACE plotjuggler_sdk::plugin_sdk)
if(NOT MSVC)
  # The fakes designate only the vtable slots they serve.
  target_compile_options(pj_test_support INTERFACE -Wno-missing-field-initializers)
endif()
