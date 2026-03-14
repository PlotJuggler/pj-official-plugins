find_package(nlohmann_json REQUIRED)
if(NOT TARGET Arrow::arrow_static)
  find_package(Arrow REQUIRED)
endif()
if(NOT TARGET Parquet::parquet_static)
  find_package(Parquet REQUIRED)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

# --- Plugin shared library ---
add_library(parquet_source_plugin SHARED src/parquet_source.cpp)
target_compile_features(parquet_source_plugin PRIVATE cxx_std_20)
target_compile_options(parquet_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parquet_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json Arrow::arrow_static Parquet::parquet_static
)

pj_embed_ui(parquet_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/dataload_parquet.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dataload_parquet_ui.hpp
  VAR_NAME kDataLoadParquetUi
)
