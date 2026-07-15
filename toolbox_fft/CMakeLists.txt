find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# kissfft is not on conda-forge — vendored under contrib/kissfft (131.1.0).
# Used by both the Conan and pixi builds (the Conan kissfft dep is dropped).
# SYSTEM include exposes contrib/ so consumers resolve <kissfft/kiss_fftr.h>.
add_library(pj_kissfft STATIC
  contrib/kissfft/kiss_fft.c
  contrib/kissfft/kiss_fftr.c
)
# The plugin collection's root project enables only CXX. kissfft's C sources have
# extern "C" header guards, so compiling them as C++ keeps the symbol linkage
# correct and avoids needing enable_language(C) at the root.
set_source_files_properties(
  contrib/kissfft/kiss_fft.c contrib/kissfft/kiss_fftr.c
  PROPERTIES LANGUAGE CXX
)
set_target_properties(pj_kissfft PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pj_kissfft SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
add_library(kissfft::kissfft ALIAS pj_kissfft)

add_library(toolbox_fft_plugin SHARED fft_plugin.cpp)

target_compile_features(toolbox_fft_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_fft_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_fft_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  kissfft::kissfft
)

target_include_directories(toolbox_fft_plugin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

pj_embed_ui(toolbox_fft_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/fft_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/fft_dialog_ui.hpp
  VAR_NAME kFftDialogUi
)

pj_embed_manifest(toolbox_fft_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/fft_manifest.hpp
  VAR_NAME kFftManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(toolbox_fft_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
