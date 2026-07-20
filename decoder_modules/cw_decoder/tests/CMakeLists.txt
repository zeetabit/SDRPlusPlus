cmake_minimum_required(VERSION 3.13)
project(cw_decoder_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Catch2 single-header (shared with main tests)
set(CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/catch2")
add_library(catch2 INTERFACE)
target_include_directories(catch2 INTERFACE ${CATCH2_DIR})

# Core include paths (needed for dsp types, buffer, taps)
set(CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/src")

# Module source
set(MOD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(cw_decoder_tests
    test_main.cpp
    test_cw_decoder.cpp
    test_tone_scanner.cpp
    test_integration.cpp
    test_channel_manager.cpp
    test_edge_cases.cpp
    test_weak_signal.cpp
    test_benchmark.cpp
    test_benchmark_multiseed.cpp
    test_registry.cpp
    test_matrix.cpp
    test_oracle.cpp
    test_detector_metrics.cpp
    test_debug_decode.cpp
    test_speed.cpp
    test_farnsworth.cpp
    test_gap_noise.cpp
    test_recording.cpp
    test_corrector.cpp
    test_conversation.cpp
    test_view_state.cpp
    test_wpm_stability.cpp
    stubs.cpp
    ${CORE_SRC}/gui/view_state.cpp
)

target_include_directories(cw_decoder_tests PRIVATE
    ${MOD_SRC}
    ${CORE_SRC}
    ${CORE_SRC}/imgui
)

# Lets the [recording] gate locate its cache regardless of the working
# directory the binary is launched from. CW_RECORDINGS_DIR overrides it.
target_compile_definitions(cw_decoder_tests PRIVATE
    CW_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(cw_decoder_tests PRIVATE catch2)

# volk and fftw3f needed for dsp
find_package(PkgConfig REQUIRED)
pkg_check_modules(VOLK REQUIRED volk)
pkg_check_modules(FFTW3F REQUIRED fftw3f)
target_include_directories(cw_decoder_tests PRIVATE ${VOLK_INCLUDE_DIRS} ${FFTW3F_INCLUDE_DIRS})
target_link_directories(cw_decoder_tests PRIVATE ${VOLK_LIBRARY_DIRS} ${FFTW3F_LIBRARY_DIRS})
target_link_libraries(cw_decoder_tests PRIVATE ${VOLK_LIBRARIES} ${FFTW3F_LIBRARIES})

enable_testing()
add_test(NAME cw_decoder_tests COMMAND cw_decoder_tests)
