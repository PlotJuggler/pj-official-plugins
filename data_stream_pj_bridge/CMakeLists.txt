find_package(Qt6 COMPONENTS WebSockets QUIET)
if(NOT Qt6WebSockets_FOUND)
  message(STATUS "Qt6::WebSockets not found — skipping pj_bridge plugin")
  return()
endif()

find_package(nlohmann_json REQUIRED)
find_package(zstd REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(pj_bridge_protocol_lib STATIC pj_bridge_protocol.cpp)
set_target_properties(pj_bridge_protocol_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pj_bridge_protocol_lib PUBLIC cxx_std_20)
target_compile_options(pj_bridge_protocol_lib PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(pj_bridge_protocol_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pj_bridge_protocol_lib PUBLIC zstd::libzstd_static)

add_library(pj_bridge_source_plugin SHARED pj_bridge_source.cpp)
target_compile_features(pj_bridge_source_plugin PRIVATE cxx_std_20)
target_compile_options(pj_bridge_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_bridge_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json pj_bridge_protocol_lib Qt6::WebSockets Qt6::Network
)

pj_embed_ui(pj_bridge_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/websocket_client.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/websocket_client_ui.hpp
  VAR_NAME kWebSocketClientUi
)
