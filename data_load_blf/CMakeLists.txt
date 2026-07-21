find_package(nlohmann_json REQUIRED)
find_package(ZLIB REQUIRED)

# Conan may expose zlib in config mode as zlib::zlib; normalize to ZLIB::ZLIB.
if(NOT TARGET ZLIB::ZLIB AND TARGET zlib::zlib)
  add_library(ZLIB::ZLIB ALIAS zlib::zlib)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

# ---------------------------------------------------------------------------
# lblf (PetStr/lblf, MIT) via CPM — DOWNLOAD_ONLY.
#
# Vector BLF reader (classic CAN + LIN + zlib-compressed LogContainers). We build
# a static lib from its two LIBRARY sources only — blf_reader.cc + print.cc.
# object_types.cc and the read*.cc files are example programs that each carry
# their own main(), so they are excluded. Pinned by commit tarball + SHA256.
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME lblf
  URL https://github.com/PetStr/lblf/archive/a453f188f67637a75e470c272cc76b475080f778.tar.gz
  URL_HASH SHA256=1ed25162dcab2316d7d8b896ac6d85f9d2f97ca2bfdf40b32165c0e4405046ff
  DOWNLOAD_ONLY YES
)
# Vendored hardening: the pinned lblf computes a log container's blob size as
# `objSize - headerSize - sizeof(LogContainer)`. A forged objSize below that
# sum underflows to ~4 GB; a forged objSize far above the file makes the same
# math (or unCompressedFileSize) resize() a multi-GB buffer before the short
# read fails — an allocation DoS from a tiny file either way. Inject a bounds
# check right after the container header is read: reject objSize outside
# [header, fileLength]. `fileLength` is the reader's own member, set from the
# real file size in the constructor. Done via string(REPLACE) (no `patch`
# executable needed on Windows CI); each pj_apply_vendored_patch FATAL_ERRORs if
# its anchor moves, so a pin bump can never silently drop a guard.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/VendoredPatch.cmake)
file(READ ${lblf_SOURCE_DIR}/blf_reader.cc _lblf_reader_cc)

set(_lblf_anchor [==[            read_logcontainer(fileStream, lc, ohb);]==])
set(_lblf_fixed [==[            if (ohb.objSize < ohb.headerSize + sizeof(blf_struct::LogContainer) || ohb.objSize > fileLength)
                {
                    throw std::runtime_error("lblf: log container size out of range (corrupt file)");
                }
            read_logcontainer(fileStream, lc, ohb);]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_anchor _lblf_fixed "data_load_blf: lblf log-container size")

# Second guard: the zlib output buffer is sized from the container's own
# unCompressedFileSize field, which is independent of objSize. Cap it against
# the compressed blob size (zlib's theoretical max expansion is ~1032:1) so a
# forged value cannot resize() a multi-GB buffer.
set(_lblf_zlib_anchor [==[                    uLong uncompressedFileSize = lc.unCompressedFileSize;
                    std::vector<uint8_t> uncompressedData {};

                    uncompressedData.resize(uncompressedFileSize);]==])
set(_lblf_zlib_fixed [==[                    uLong uncompressedFileSize = lc.unCompressedFileSize;
                    if (uncompressedFileSize > static_cast<uLong>(compressedBlobSize) * 1100 + 65536)
                        {
                            throw std::runtime_error("lblf: implausible uncompressed container size (corrupt file)");
                        }
                    std::vector<uint8_t> uncompressedData {};

                    uncompressedData.resize(uncompressedFileSize);]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_zlib_anchor _lblf_zlib_fixed "data_load_blf: lblf zlib output size")

# Third guard: blf_reader::data() computes an inner object's payload length as
# `objSize - sizeof(BaseHeader) + objSize % 4`. A forged inner objSize below
# sizeof(BaseHeader) underflows that size_t, and the following pop loop then
# reads past an emptied deque (front()/pop_front() on empty = undefined). Bail
# to the end-of-stream sentinel (0x888, already used above) when the object is
# undersized or the stream ran short.
set(_lblf_data_anchor [==[    if (logcontainer_que.size() < size_of_data)
        {
            fill_deque();
        }

    result.payload.clear();
    result.payload.reserve(size_of_data);]==])
set(_lblf_data_fixed [==[    if (logcontainer_que.size() < size_of_data)
        {
            fill_deque();
        }

    if (result.base_header.objSize < sizeof(result.base_header) || logcontainer_que.size() < size_of_data)
        {
            result.base_header.objectType = static_cast<ObjectType_e>(0x888);
            return result;
        }

    result.payload.clear();
    result.payload.reserve(size_of_data);]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_data_anchor _lblf_data_fixed "data_load_blf: lblf inner-object size")

set(_lblf_patched_dir ${CMAKE_CURRENT_BINARY_DIR}/lblf_patched)
file(CONFIGURE OUTPUT ${_lblf_patched_dir}/blf_reader.cc CONTENT "${_lblf_reader_cc}" @ONLY)

add_library(blf STATIC
  ${_lblf_patched_dir}/blf_reader.cc
  ${lblf_SOURCE_DIR}/print.cc
)
set_target_properties(blf PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(blf PRIVATE cxx_std_20)
# SYSTEM so lblf's headers don't trip the plugin's -Werror.
target_include_directories(blf SYSTEM PUBLIC ${lblf_SOURCE_DIR})
target_link_libraries(blf PRIVATE ZLIB::ZLIB)

# ---------------------------------------------------------------------------
# Internal static lib: BLF -> CanFrame adapter, shared by the plugin + tests.
# blf is PRIVATE so lblf headers stay out of consumers (blf_frames.hpp is
# lblf-free).
# ---------------------------------------------------------------------------
add_library(blf_core STATIC blf_frames.cpp)
set_target_properties(blf_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(blf_core PUBLIC cxx_std_20)
target_compile_options(blf_core PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(blf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(blf_core PUBLIC plotjuggler_sdk::base PRIVATE blf)

# ---------------------------------------------------------------------------
# Plugin shared library
# ---------------------------------------------------------------------------
add_library(blf_source_plugin SHARED blf_source.cpp blf_dialog.cpp)
target_compile_features(blf_source_plugin PRIVATE cxx_std_20)
target_compile_options(blf_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(blf_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  blf_core
  pj_can_dbc
  nlohmann_json::nlohmann_json)

pj_embed_manifest(blf_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/blf_manifest.hpp
  VAR_NAME kBlfManifest
)

pj_emit_plugin_manifest(blf_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_embed_ui(blf_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/blf_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/blf_dialog_ui.hpp
  VAR_NAME kBlfDialogUi
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(blf_source_test
  tests/blf_frames_test.cpp
  tests/blf_dialog_test.cpp
  blf_dialog.cpp
)
target_compile_features(blf_source_test PRIVATE cxx_std_20)
target_compile_options(blf_source_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(blf_source_test PRIVATE
  BLF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data")
# blf_dialog.cpp includes the headers generated for the plugin target above.
target_include_directories(blf_source_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(blf_source_test PRIVATE
  plotjuggler_sdk::plugin_host
  blf_core
  pj_can_dbc
  nlohmann_json::nlohmann_json
  GTest::gtest_main)
add_test(NAME blf_source_test COMMAND blf_source_test)
