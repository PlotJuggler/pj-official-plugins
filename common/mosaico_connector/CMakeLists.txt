# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MIT

# Shared Mosaico connector components. Both are Flight-free: the ingest target
# translates Arrow data into host datasets, while descriptor_import owns the
# durable layout-reimport descriptor/cache/artifact contract.

set(PJ_MOSAICO_CONNECTOR_ARROW_TARGET "")
foreach(_candidate arrow::arrow Arrow::arrow_static Arrow::arrow_shared Arrow::arrow)
  if(TARGET ${_candidate})
    set(PJ_MOSAICO_CONNECTOR_ARROW_TARGET ${_candidate})
    break()
  endif()
endforeach()
if(NOT PJ_MOSAICO_CONNECTOR_ARROW_TARGET)
  find_package(Arrow QUIET CONFIG)
  foreach(_candidate arrow::arrow Arrow::arrow_static Arrow::arrow_shared Arrow::arrow)
    if(TARGET ${_candidate})
      set(PJ_MOSAICO_CONNECTOR_ARROW_TARGET ${_candidate})
      break()
    endif()
  endforeach()
endif()
if(NOT PJ_MOSAICO_CONNECTOR_ARROW_TARGET)
  message(STATUS "mosaico_connector: Arrow not found, skipping")
  return()
endif()

add_subdirectory(ingest)
add_subdirectory(descriptor_import)
