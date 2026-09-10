# HardenPluginExports.cmake
#
# pj_harden_plugin_exports(<target>
#     REQUIRED_EXPORTS <family getters...>
#     [EXTRA_EXPORTS <additional symbols...>])
#
# Restricts a plugin DSO's dynamic exports to the shared ABI allowlist
# (plugin_exports.map.in, configured per target) and wires the post-build gate
# that fails the build when any STB_GNU_UNIQUE symbol leaks into .dynsym or a
# required boot export goes missing (see CheckElfPluginExports.cmake for the
# full rationale).
#
# REQUIRED_EXPORTS lists the entry points this plugin must export
# (e.g. PJ_get_toolbox_vtable PJ_get_dialog_vtable); pj_plugin_abi_version is
# always required and added automatically. EXTRA_EXPORTS adds target-specific
# symbols to the allowlist beyond the standard family set — e.g. the ros2
# proxy's documented PJ_get_proxy_last_error diagnostic hook, or the ros2
# inner payload's private PJ_ros2_inner_* getters (list them in
# REQUIRED_EXPORTS too when the gate should enforce their presence).
#
# A version script — rather than --exclude-libs,ALL — because the boot symbol
# pj_plugin_abi_version is a weak header-emitted definition that static-archive
# members on the link line also carry: hiding the archive copy would demote
# every copy via ELF visibility merging, while the version script selects by
# final symbol name after resolution.
#
# Linux uses a version script (STB_GNU_UNIQUE gate + allowlist). macOS uses
# -exported_symbols_list (plugin_exports_macos.list.in): Mach-O has no
# STB_GNU_UNIQUE, but exported weak C++ symbols join dyld's process-wide weak
# coalescing on dlopen, which can rebind part of a plugin's typeinfo references
# to the host's copy and break libc++'s pointer-based typeid identity (std::any
# in the plugin resolving to "unknown type"). Windows needs nothing: MSVC
# exports nothing without dllexport.

function(pj_harden_plugin_exports TARGET)
  set(_options)
  set(_oneValueArgs)
  set(_multiValueArgs REQUIRED_EXPORTS EXTRA_EXPORTS)
  cmake_parse_arguments(ARG "${_options}" "${_oneValueArgs}" "${_multiValueArgs}" ${ARGN})

  if(NOT ARG_REQUIRED_EXPORTS)
    message(FATAL_ERROR "pj_harden_plugin_exports(${TARGET}): REQUIRED_EXPORTS is required")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PJ_EXTRA_EXPORT_LINES "")
    foreach(_symbol IN LISTS ARG_EXTRA_EXPORTS)
      string(APPEND PJ_EXTRA_EXPORT_LINES "_${_symbol}\n")
    endforeach()
    set(_list "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_exports.exp")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_exports_macos.list.in" "${_list}" @ONLY)
    target_link_options(${TARGET} PRIVATE "LINKER:-exported_symbols_list,${_list}")
    set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${_list}")
    return()
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
  endif()

  set(PJ_EXTRA_EXPORT_LINES "")
  foreach(_symbol IN LISTS ARG_EXTRA_EXPORTS)
    string(APPEND PJ_EXTRA_EXPORT_LINES "    ${_symbol};\n")
  endforeach()
  set(_map "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_exports.map")
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_exports.map.in" "${_map}" @ONLY)

  set(_checker "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckElfPluginExports.cmake")
  target_link_options(${TARGET} PRIVATE "LINKER:--version-script=${_map}")
  # Re-link (and thereby re-gate) when the generated map or the checker itself
  # changes; configure_file already re-runs CMake when the template changes.
  set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${_map}" "${_checker}")

  list(PREPEND ARG_REQUIRED_EXPORTS pj_plugin_abi_version)
  list(JOIN ARG_REQUIRED_EXPORTS "," _required)
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DPLUGIN_SO=$<TARGET_FILE:${TARGET}>
      -DREQUIRED_EXPORTS=${_required}
      -DNM_TOOL=${CMAKE_NM}
      -P ${_checker}
    COMMENT "${TARGET}: verifying export allowlist (no STB_GNU_UNIQUE leaks)"
    VERBATIM)
endfunction()
