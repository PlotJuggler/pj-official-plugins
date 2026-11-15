# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/laser_scan/wasm.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/grid_map/wasm.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/pointcloud_color/wasm.cmake")

pj_wasm_official_plugin(pj_wasm_ros_parser
  SOURCES ros_parser.cpp ros_specializations.cpp ros_builtin_object_handlers.cpp ros_marker_handlers.cpp
          ros_yolo_handlers.cpp ros_parser_plugin.cpp
  MANIFEST ros_manifest.hpp=kRosManifest
  UI ros_parser_options.ui=kRosParserOptionsUi
  MESSAGE_PARSER RosParserPlugin DIALOG RosParserDialog
  LINKS nlohmann_json::nlohmann_json rosx_introspection data_tamer_parser
        pj_laser_scan pj_grid_map pj_pointcloud_color)
