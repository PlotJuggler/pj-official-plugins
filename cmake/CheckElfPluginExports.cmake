# CheckElfPluginExports.cmake
#
# Post-build gate for ELF plugin DSOs, run in CMake script mode:
#
#   cmake -DPLUGIN_SO=<path/to/plugin.so>
#         -DREQUIRED_EXPORTS=<sym1,sym2,...>
#         -P CheckElfPluginExports.cmake
#
# Fails the build when:
#   1. the DSO exports any STB_GNU_UNIQUE symbol ('u' in `nm -D`). Exported
#      unique symbols are process-global despite RTLD_LOCAL: glibc pins the
#      first DSO providing such a name (dlclose stops unmapping it) and a
#      second copy of the plugin loaded from another path binds into the first
#      copy's statics — skipped constructors and cross-build state mixing.
#   2. any of REQUIRED_EXPORTS (comma-separated) is missing from the dynamic
#      symbol table — catches an over-aggressive export allowlist that would
#      make the host reject the plugin at the ABI handshake.

if(NOT PLUGIN_SO OR NOT REQUIRED_EXPORTS)
  message(FATAL_ERROR "CheckElfPluginExports: PLUGIN_SO and REQUIRED_EXPORTS are required")
endif()

execute_process(
  COMMAND nm -D "${PLUGIN_SO}"
  OUTPUT_VARIABLE _dynsym
  ERROR_VARIABLE _nm_err
  RESULT_VARIABLE _nm_rc
)
if(NOT _nm_rc EQUAL 0)
  message(FATAL_ERROR "CheckElfPluginExports: nm -D failed on ${PLUGIN_SO}: ${_nm_err}")
endif()

string(REGEX MATCHALL "[^\n]* u [^\n]*" _unique_syms "${_dynsym}")
list(LENGTH _unique_syms _unique_count)
if(_unique_count GREATER 0)
  list(SUBLIST _unique_syms 0 5 _unique_sample)
  list(JOIN _unique_sample "\n  " _unique_sample_text)
  message(FATAL_ERROR
    "CheckElfPluginExports: ${PLUGIN_SO} exports ${_unique_count} STB_GNU_UNIQUE "
    "symbol(s); they must all be localized (see plugin_exports.map). First few:\n"
    "  ${_unique_sample_text}")
endif()

string(REPLACE "," ";" _required "${REQUIRED_EXPORTS}")
foreach(_symbol IN LISTS _required)
  if(NOT _dynsym MATCHES "[ \t][TWVDB][ \t]+${_symbol}(\n|$)")
    message(FATAL_ERROR
      "CheckElfPluginExports: required export \"${_symbol}\" is missing from "
      "${PLUGIN_SO} — the host would reject the plugin at the ABI handshake. "
      "Check the version script's global list.")
  endif()
endforeach()

message(STATUS "CheckElfPluginExports: ${PLUGIN_SO} — 0 unique symbols, all required exports present")
