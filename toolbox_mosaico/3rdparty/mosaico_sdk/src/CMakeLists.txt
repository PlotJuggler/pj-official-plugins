set(SDK_SOURCES
    flight/metadata.cpp
    flight/connection_pool.cpp
    flight/query.cpp
    flight/mosaico_client.cpp
)

add_library(mosaico_sdk STATIC ${SDK_SOURCES})

# Required: the SDK is linked into a SHARED plugin, so its objects must
# be position-independent. PIC is implicit for SHARED targets but not
# for STATIC, so we set it explicitly here.
set_target_properties(mosaico_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(mosaico_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Resolve Arrow + Arrow Flight targets. Conan's arrow package publishes a
# single `arrow::arrow` target that bundles flight_rpc when the conanfile
# enables that option; upstream Arrow CMake creates Arrow::arrow_static
# + ArrowFlight::arrow_flight_static. Accept either shape so this SDK
# builds against both.
set(_mosaico_arrow_target "")
foreach(_candidate arrow::arrow Arrow::arrow_static Arrow::arrow_shared Arrow::arrow)
    if(TARGET ${_candidate})
        set(_mosaico_arrow_target ${_candidate})
        break()
    endif()
endforeach()

set(_mosaico_arrow_flight_target "")
foreach(_candidate
        ArrowFlight::arrow_flight_static
        arrow::arrow_flight
        ArrowFlight::arrow_flight
        arrow::arrow_flight_static)
    if(TARGET ${_candidate})
        set(_mosaico_arrow_flight_target ${_candidate})
        break()
    endif()
endforeach()

if(NOT _mosaico_arrow_target OR NOT _mosaico_arrow_flight_target)
    message(FATAL_ERROR
        "[MosaicoSDK] Arrow (${_mosaico_arrow_target}) or Arrow Flight "
        "(${_mosaico_arrow_flight_target}) target not found. Enable "
        "arrow:with_flight_rpc=True in your Conan profile, or install "
        "Arrow with Flight from upstream.")
endif()

target_link_libraries(mosaico_sdk PUBLIC
    ${_mosaico_arrow_target}
    ${_mosaico_arrow_flight_target}
    nlohmann_json::nlohmann_json
)

# Conan's CMakeDeps propagates ARROW_STATIC via Arrow::arrow_static but does
# not propagate ARROW_FLIGHT_STATIC via ArrowFlight::arrow_flight_static.
# Without ARROW_FLIGHT_STATIC, Arrow's flight headers default to
# __declspec(dllimport) on MSVC, producing LNK2019 on inline/implicit ctors.
# Pin the defines directly so they reach this target's .cpp files and
# propagate (PUBLIC) to consumers like ToolboxMosaico.
target_compile_definitions(mosaico_sdk PUBLIC ARROW_STATIC ARROW_FLIGHT_STATIC)

target_compile_features(mosaico_sdk PUBLIC cxx_std_17)
