find_package(nlohmann_json REQUIRED)
find_package(cppzmq REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(zmq_source_plugin SHARED zmq_source.cpp)
target_compile_features(zmq_source_plugin PRIVATE cxx_std_20)
target_compile_options(zmq_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(zmq_source_plugin PRIVATE pj_base pj_dialog_sdk nlohmann_json::nlohmann_json cppzmq)

pj_embed_ui(zmq_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/datastream_zmq.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_zmq_ui.hpp
  VAR_NAME kDataStreamZmqUi
)
