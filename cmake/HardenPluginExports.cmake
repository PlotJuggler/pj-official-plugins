# HardenPluginExports.cmake
#
# pj_harden_plugin_exports(<target> REQUIRED_EXPORTS <family getters...>)
#
# Restricts a plugin DSO's dynamic exports to the shared ABI allowlist
# (plugin_exports.map) and wires the post-build gate that fails the build when
# any STB_GNU_UNIQUE symbol leaks into .dynsym or a required boot export goes
# missing (see CheckElfPluginExports.cmake for the full rationale).
#
# REQUIRED_EXPORTS lists the family entry points this plugin must export
# (e.g. PJ_get_toolbox_vtable PJ_get_dialog_vtable); pj_plugin_abi_version is
# always required and added automatically.
#
# A version script — rather than --exclude-libs,ALL — because the boot symbol
# pj_plugin_abi_version is a weak header-emitted definition that static-archive
# members on the link line also carry: hiding the archive copy would demote
# every copy via ELF visibility merging, while the version script selects by
# final symbol name after resolution.
#
# Linux-only by design: PE and Mach-O have no STB_GNU_UNIQUE, and MSVC exports
# nothing without dllexport.

function(pj_harden_plugin_exports TARGET)
  set(_options)
  set(_oneValueArgs)
  set(_multiValueArgs REQUIRED_EXPORTS)
  cmake_parse_arguments(ARG "${_options}" "${_oneValueArgs}" "${_multiValueArgs}" ${ARGN})

  if(NOT ARG_REQUIRED_EXPORTS)
    message(FATAL_ERROR "pj_harden_plugin_exports(${TARGET}): REQUIRED_EXPORTS is required")
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
  endif()

  set(_map "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_exports.map")
  target_link_options(${TARGET} PRIVATE "LINKER:--version-script=${_map}")
  set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${_map}")

  list(PREPEND ARG_REQUIRED_EXPORTS pj_plugin_abi_version)
  list(JOIN ARG_REQUIRED_EXPORTS "," _required)
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DPLUGIN_SO=$<TARGET_FILE:${TARGET}>
      -DREQUIRED_EXPORTS=${_required}
      -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckElfPluginExports.cmake
    COMMENT "${TARGET}: verifying export allowlist (no STB_GNU_UNIQUE leaks)"
    VERBATIM)
endfunction()
