# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_fft
  SOURCES fft_plugin.cpp fft_algorithm.cpp
  MANIFEST fft_manifest.hpp=kFftManifest
  UI fft_dialog.ui=kFftDialogUi
  TOOLBOX FFTToolbox DIALOG FFTDialog
  LINKS nlohmann_json::nlohmann_json pocketfft::pocketfft)
# One FFT per click, and the browser build has no threads for pocketfft's pool.
target_compile_definitions(pj_wasm_fft PRIVATE POCKETFFT_NO_MULTITHREADING)
