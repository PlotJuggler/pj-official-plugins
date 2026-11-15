# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/base64/wasm.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/laser_scan/wasm.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/grid_map/wasm.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/pointcloud_color/wasm.cmake")

pj_wasm_official_plugin(pj_wasm_protobuf_parser
  SOURCES protobuf_parser.cpp foxglove_pointcloud_codec.cpp foxglove_object_codecs.cpp
          foxglove_voxelgrid_codec.cpp foxglove_grid_codec.cpp
  MANIFEST protobuf_manifest.hpp=kProtobufManifest
  UI protobuf_parser_options.ui=kProtobufParserOptionsUi
  MESSAGE_PARSER ProtobufParser DIALOG ProtobufParserDialog
  LINKS protobuf::libprotobuf nlohmann_json::nlohmann_json pj_base64 pj_laser_scan pj_grid_map pj_pointcloud_color)
