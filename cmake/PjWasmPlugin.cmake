# WebAssembly build of the official plugins, as one static library each linked into the
# browser build of PlotJuggler (which cannot dlopen). Only that host includes this file and
# the plugins' wasm.cmake files, under EMSCRIPTEN; the native and Conan builds never read them.
#
# Contract with the host build:
# - It includes `<plugin dir>/wasm.cmake` for each plugin it ships, in its registration order.
#   A wasm.cmake declares one plugin with pj_wasm_official_plugin() and adds the shared
#   `common/` libraries it links with pj_wasm_common_library(), which reuses their native
#   CMakeLists (common/can_dbc, which downloads its sources natively, has a wasm.cmake instead).
# - Before that it provides, under the names the plugins' native CMakeLists use:
#   plotjuggler_sdk::plugin_sdk and ::base, nlohmann_json::nlohmann_json, date::date,
#   LZ4::lz4_static, zstd::libzstd_static, protobuf::libprotobuf, rosx_introspection,
#   data_tamer_parser (DataTamer's header-only parser), ulog_cpp::ulog_cpp, pocketfft::pocketfft,
#   nanoarrow::nanoarrow, Arrow::arrow_static, Parquet::parquet_static, ZLIB::ZLIB, EXPAT::EXPAT,
#   Qt6::Widgets, for each plugin it enables that needs them; and the source trees
#   date_SOURCE_DIR, dbc_parser_cpp_SOURCE_DIR, dbc_fast_float_SOURCE_DIR,
#   lblf_SOURCE_DIR and mdflib_SOURCE_DIR (FetchContent's variables) for the plugins that
#   compile from them. PJ_WARNING_FLAGS, when set, is added to each plugin's compile options.
# - pj_wasm_official_plugin() appends the target to the GLOBAL property PJ_WASM_OFFICIAL_PLUGINS
#   and stores its registration in the target properties PJ_WASM_PLUGIN_KIND (data_source,
#   message_parser, toolbox or widget), PJ_WASM_PLUGIN_SYMBOL and PJ_WASM_PLUGIN_DIALOG (empty
#   without a dialog). The host links each target and turns them into its static registration:
#   pj_static_get_<kind>_vtable_<SYMBOL>() and pj_static_get_dialog_vtable_<DIALOG>(), the
#   getters the PJ_*_PLUGIN / PJ_DIALOG_PLUGIN macros define in a static build.
include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/EmbedManifest.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/EmbedUi.cmake")

# The compile settings every official WASM library shares.
function(pj_wasm_finalize_target target)
  target_compile_options(${target} PRIVATE ${PJ_WARNING_FLAGS})
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# pj_wasm_common_library(<lib>...): adds common/<lib>'s native CMakeLists, which defines
# pj_<lib>, unless an earlier plugin already did.
function(pj_wasm_common_library)
  foreach(_lib IN LISTS ARGN)
    if(NOT TARGET pj_${_lib})
      add_subdirectory("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../common/${_lib}" "${CMAKE_BINARY_DIR}/pj_wasm_common/${_lib}")
    endif()
  endforeach()
endfunction()

# pj_wasm_official_plugin(<target>
#     SOURCES <file>...                     relative to the calling wasm.cmake's directory (or absolute)
#     MANIFEST <header>=<Var>               that directory's manifest.json
#     [UI <file.ui>=<Var>...]               each into <ui stem>_ui.hpp
#     [DATA_SOURCE|MESSAGE_PARSER|TOOLBOX|WIDGET <Symbol>] [DIALOG <Symbol>]
#     [INCLUDES <dir>...]                   private, relative to that directory (or absolute)
#     [LINKS <target>...])                  private, besides plotjuggler_sdk::plugin_sdk; each must exist
# The plugin directory is a private include directory; generated headers go to a directory of
# the target's own.
function(pj_wasm_official_plugin target)
  cmake_parse_arguments(ARG "" "MANIFEST;DATA_SOURCE;MESSAGE_PARSER;TOOLBOX;WIDGET;DIALOG"
                        "SOURCES;UI;INCLUDES;LINKS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES OR NOT ARG_MANIFEST)
    message(FATAL_ERROR "pj_wasm_official_plugin(${target}): MANIFEST is required; unexpected "
                        "'${ARG_UNPARSED_ARGUMENTS}', empty '${ARG_KEYWORDS_MISSING_VALUES}'")
  endif()
  # A missing target without `::` would otherwise link as a plain -l<name>.
  foreach(_link IN LISTS ARG_LINKS)
    if(NOT TARGET ${_link})
      message(FATAL_ERROR "pj_wasm_official_plugin(${target}): LINKS target ${_link} does not exist")
    endif()
  endforeach()
  set(_dir "${CMAKE_CURRENT_LIST_DIR}")
  set(_kind "")
  foreach(_k IN ITEMS DATA_SOURCE MESSAGE_PARSER TOOLBOX WIDGET)
    if(ARG_${_k})
      if(_kind)
        message(FATAL_ERROR "pj_wasm_official_plugin(${target}): more than one plugin kind")
      endif()
      string(TOLOWER "${_k}" _kind)
      set(_symbol "${ARG_${_k}}")
    endif()
  endforeach()
  if(NOT _kind)
    message(FATAL_ERROR "pj_wasm_official_plugin(${target}): DATA_SOURCE, MESSAGE_PARSER, TOOLBOX or WIDGET is required")
  endif()

  foreach(_list IN ITEMS SOURCES INCLUDES)
    set(_absolute "")
    foreach(_path IN LISTS ARG_${_list})
      cmake_path(ABSOLUTE_PATH _path BASE_DIRECTORY "${_dir}" NORMALIZE)
      list(APPEND _absolute "${_path}")
    endforeach()
    set(ARG_${_list} ${_absolute})
  endforeach()

  add_library(${target} STATIC ${ARG_SOURCES})
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_include_directories(${target} PRIVATE "${_dir}" ${ARG_INCLUDES})
  target_link_libraries(${target} PRIVATE plotjuggler_sdk::plugin_sdk ${ARG_LINKS})

  set(_generated "${CMAKE_CURRENT_BINARY_DIR}/pj_wasm_plugins/${target}")
  string(REPLACE "=" ";" _pair "${ARG_MANIFEST}")
  list(GET _pair 0 _header)
  list(GET _pair 1 _var)
  pj_embed_manifest(${target} MANIFEST_FILE "${_dir}/manifest.json" HEADER "${_generated}/${_header}" VAR_NAME ${_var})
  foreach(_ui IN LISTS ARG_UI)
    string(REPLACE "=" ";" _pair "${_ui}")
    list(GET _pair 0 _file)
    list(GET _pair 1 _var)
    get_filename_component(_stem "${_file}" NAME_WE)
    pj_embed_ui(${target} UI_FILE "${_dir}/${_file}" HEADER "${_generated}/${_stem}_ui.hpp" VAR_NAME ${_var})
  endforeach()
  pj_wasm_finalize_target(${target})

  set_target_properties(${target} PROPERTIES
    PJ_WASM_PLUGIN_KIND "${_kind}" PJ_WASM_PLUGIN_SYMBOL "${_symbol}" PJ_WASM_PLUGIN_DIALOG "${ARG_DIALOG}")
  set_property(GLOBAL APPEND PROPERTY PJ_WASM_OFFICIAL_PLUGINS ${target})
endfunction()
