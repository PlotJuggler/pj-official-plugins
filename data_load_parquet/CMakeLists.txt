find_package(nlohmann_json REQUIRED)
find_package(date REQUIRED)
if(NOT TARGET Arrow::arrow_static)
  find_package(Arrow REQUIRED)
endif()
if(NOT TARGET Parquet::parquet_static)
  find_package(Parquet REQUIRED)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(parquet_source_plugin SHARED parquet_source.cpp)
target_compile_features(parquet_source_plugin PRIVATE cxx_std_20)
target_compile_options(parquet_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parquet_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json date::date-tz Arrow::arrow_static Parquet::parquet_static
)

pj_embed_ui(parquet_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/dataload_parquet.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dataload_parquet_ui.hpp
  VAR_NAME kDataLoadParquetUi
)

pj_embed_manifest(parquet_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/parquet_manifest.hpp
  VAR_NAME kParquetManifest
)

# --- Unit test for Parquet helpers ---
add_executable(parquet_helpers_test tests/parquet_helpers_test.cpp)
target_compile_features(parquet_helpers_test PRIVATE cxx_std_20)
target_compile_options(parquet_helpers_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(parquet_helpers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(parquet_helpers_test PRIVATE
  pj_base Arrow::arrow_static GTest::gtest_main
)
add_test(NAME parquet_helpers_test COMMAND parquet_helpers_test)
