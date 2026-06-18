# Provider-agnostic dependency linking for the Conan->pixi transition.
#
# Several third-party deps expose DIFFERENT CMake target names under Conan
# (static archives, e.g. Arrow::arrow_static) versus conda-forge (shared libs,
# e.g. Arrow::arrow_shared) — and a few ship no CMake target at all on
# conda-forge (lz4-c, asio: pkg-config only). These helpers locate the dep under
# whichever provider is active and link the correct target, so the SAME plugin
# CMakeLists builds under both. Under Conan they link the identical *_static
# targets used before this layer existed (no behavior change).
#
# Usage:  pj_target_link_arrow(my_target PRIVATE)   # or PUBLIC / INTERFACE
include_guard(GLOBAL)

function(pj_target_link_arrow tgt vis)
  if(NOT TARGET Arrow::arrow_static AND NOT TARGET Arrow::arrow_shared)
    find_package(Arrow CONFIG REQUIRED)
  endif()
  if(TARGET Arrow::arrow_static)
    target_link_libraries(${tgt} ${vis} Arrow::arrow_static)
  else()
    target_link_libraries(${tgt} ${vis} Arrow::arrow_shared)
  endif()
endfunction()

function(pj_target_link_parquet tgt vis)
  if(NOT TARGET Parquet::parquet_static AND NOT TARGET Parquet::parquet_shared)
    find_package(Parquet CONFIG REQUIRED)
  endif()
  if(TARGET Parquet::parquet_static)
    target_link_libraries(${tgt} ${vis} Parquet::parquet_static)
  else()
    target_link_libraries(${tgt} ${vis} Parquet::parquet_shared)
  endif()
endfunction()

function(pj_target_link_zstd tgt vis)
  if(NOT TARGET zstd::libzstd_static AND NOT TARGET zstd::libzstd AND NOT TARGET zstd::libzstd_shared)
    find_package(zstd CONFIG REQUIRED)
  endif()
  if(TARGET zstd::libzstd_static)
    target_link_libraries(${tgt} ${vis} zstd::libzstd_static)   # Conan
  elseif(TARGET zstd::libzstd)
    target_link_libraries(${tgt} ${vis} zstd::libzstd)          # conda-forge (alias -> shared)
  else()
    target_link_libraries(${tgt} ${vis} zstd::libzstd_shared)   # custom/distro zstd (shared only)
  endif()
endfunction()

function(pj_target_link_lz4 tgt vis)
  if(TARGET LZ4::lz4_static)
    target_link_libraries(${tgt} ${vis} LZ4::lz4_static)        # Conan
    return()
  endif()
  if(TARGET lz4::lz4)
    target_link_libraries(${tgt} ${vis} lz4::lz4)
    return()
  endif()
  # conda-forge lz4-c ships no CMake config — consume via pkg-config. Guard the
  # imported-target creation so repeated calls (e.g. mcap links lz4 thrice) and
  # calls from multiple subdirectories don't re-create PkgConfig::PJ_LZ4.
  if(NOT TARGET PkgConfig::PJ_LZ4)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PJ_LZ4 REQUIRED IMPORTED_TARGET liblz4)
  endif()
  target_link_libraries(${tgt} ${vis} PkgConfig::PJ_LZ4)
endfunction()

function(pj_target_link_asio tgt vis)
  if(TARGET asio::asio)
    target_link_libraries(${tgt} ${vis} asio::asio)             # Conan
    return()
  endif()
  # conda-forge asio is header-only with no CMake target — pkg-config supplies the
  # include dir. Guard the imported-target creation against repeated/cross-subdir calls.
  if(NOT TARGET PkgConfig::PJ_ASIO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PJ_ASIO REQUIRED IMPORTED_TARGET asio)
  endif()
  target_link_libraries(${tgt} ${vis} PkgConfig::PJ_ASIO)
endfunction()

function(pj_target_link_paho_mqtt tgt vis)
  if(NOT TARGET PahoMqttCpp::paho-mqttpp3-static AND NOT TARGET PahoMqttCpp::paho-mqttpp3)
    find_package(PahoMqttCpp CONFIG REQUIRED)
  endif()
  if(TARGET PahoMqttCpp::paho-mqttpp3-static)
    target_link_libraries(${tgt} ${vis} PahoMqttCpp::paho-mqttpp3-static)  # Conan
  else()
    target_link_libraries(${tgt} ${vis} PahoMqttCpp::paho-mqttpp3)         # conda-forge
  endif()
endfunction()

# Availability probe for the graceful-skip in common/arrow_helpers (some build
# configs, e.g. the ROS2 proxy leg, have no Arrow dep at all).
function(pj_arrow_available out)
  if(TARGET Arrow::arrow_static OR TARGET Arrow::arrow_shared)
    set(${out} TRUE PARENT_SCOPE)
    return()
  endif()
  find_package(Arrow QUIET CONFIG)
  if(TARGET Arrow::arrow_static OR TARGET Arrow::arrow_shared)
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()
