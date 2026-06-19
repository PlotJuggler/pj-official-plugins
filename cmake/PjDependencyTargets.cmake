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
  # Conan ships an lz4 CMake config (LZ4::lz4_static); conda-forge lz4-c ships none.
  # Find it here (QUIET, not REQUIRED) so plugins don't need their own find_package.
  if(NOT TARGET LZ4::lz4_static AND NOT TARGET lz4::lz4)
    find_package(lz4 QUIET CONFIG)
  endif()
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
  # Conan ships an asio CMake config (asio::asio); conda-forge asio is header-only
  # with none. Find it here (QUIET) so plugins don't need their own find_package.
  if(NOT TARGET asio::asio)
    find_package(asio QUIET CONFIG)
  endif()
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

function(pj_target_link_lua tgt vis)
  if(NOT TARGET lua::lua)
    find_package(lua QUIET CONFIG)   # Conan provides lua::lua
  endif()
  if(TARGET lua::lua)
    target_link_libraries(${tgt} ${vis} lua::lua)             # Conan
    return()
  endif()
  # conda-forge lua ships only the builtin FindLua module (LUA_LIBRARIES /
  # LUA_INCLUDE_DIR) with no namespaced target — wrap it in an imported target.
  if(NOT TARGET pj_lua)
    find_package(Lua REQUIRED)
    add_library(pj_lua INTERFACE IMPORTED GLOBAL)
    set_target_properties(pj_lua PROPERTIES
      INTERFACE_LINK_LIBRARIES "${LUA_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}")
  endif()
  target_link_libraries(${tgt} ${vis} pj_lua)
endfunction()

# Link FFmpeg libav* components by name, e.g.
#   pj_target_link_ffmpeg(my_target PRIVATE avformat avutil)
# Conan ships an ffmpeg CMake config (ffmpeg::avformat, ffmpeg::avcodec, ...);
# conda-forge ffmpeg ships pkg-config (.pc) ONLY, no CMake config. The helper
# links the Conan targets when present, else builds IMPORTED targets from the
# libav<component> pkg-config modules. GLOBAL + a per-component TARGET guard make
# this safe when several subdirectories link the same component (e.g. data_load_mp4
# and common/pj_video_demux both link avformat/avutil in one build).
function(pj_target_link_ffmpeg tgt vis)
  if(NOT TARGET ffmpeg::avutil)
    find_package(ffmpeg QUIET CONFIG)
  endif()
  if(TARGET ffmpeg::avutil)
    foreach(_c ${ARGN})
      target_link_libraries(${tgt} ${vis} ffmpeg::${_c})            # Conan
    endforeach()
    return()
  endif()
  find_package(PkgConfig REQUIRED)
  foreach(_c ${ARGN})
    if(NOT TARGET PkgConfig::PJ_FF_${_c})
      pkg_check_modules(PJ_FF_${_c} REQUIRED IMPORTED_TARGET GLOBAL lib${_c})
    endif()
    target_link_libraries(${tgt} ${vis} PkgConfig::PJ_FF_${_c})     # conda-forge
  endforeach()
endfunction()

# Availability probe for the graceful-skip in common/pj_video_demux (builds that
# exercise only non-ffmpeg plugins have no ffmpeg dep at all). Detects ffmpeg under
# BOTH providers — Conan's CMake config OR conda-forge's pkg-config — so the skip
# fires only when ffmpeg is genuinely absent, not merely missing a CMake config.
function(pj_ffmpeg_available out)
  if(TARGET ffmpeg::avutil)
    set(${out} TRUE PARENT_SCOPE)
    return()
  endif()
  find_package(ffmpeg QUIET CONFIG)
  if(TARGET ffmpeg::avutil)
    set(${out} TRUE PARENT_SCOPE)
    return()
  endif()
  # pkg-config fallback (conda-forge ffmpeg has no CMake config). Gated on an active
  # conda environment so the Conan aggregate build — which has NO ffmpeg dep and must
  # SKIP pj_video_demux — never silently picks up a SYSTEM ffmpeg from the host's
  # pkg-config. Under Conan, ffmpeg arrives only as a CMake config (handled above).
  if(DEFINED ENV{CONDA_PREFIX})
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(PJ_FF_PROBE QUIET libavutil)
      if(PJ_FF_PROBE_FOUND)
        set(${out} TRUE PARENT_SCOPE)
        return()
      endif()
    endif()
  endif()
  set(${out} FALSE PARENT_SCOPE)
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
