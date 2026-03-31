cmake_minimum_required(VERSION 3.13)
project(sdrpp_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Catch2 single-header
add_library(catch2 INTERFACE)
target_include_directories(catch2 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/catch2)

# Core include paths
set(CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../core/src")

# Unit tests
add_executable(sdrpp_unit_tests
    unit/test_main.cpp
    unit/test_event_bus.cpp
    unit/test_service_registry.cpp
    unit/test_api_version.cpp
    unit/test_thread_pool.cpp
    unit/test_shared_buffer.cpp
    unit/test_gui_math.cpp
    unit/test_view_state.cpp
    unit/test_vfo_handler.cpp
    # Integration tests
    integration/test_scheduled_dsp.cpp
    # Stubs for symbols needed by core sources in the test binary
    unit/stubs.cpp
    # Core sources needed by tests
    ${CORE_SRC}/utils/flog.cpp
    ${CORE_SRC}/gui/view_state.cpp
    ${CORE_SRC}/gui/vfo_handler.cpp
)

target_include_directories(sdrpp_unit_tests PRIVATE
    ${CORE_SRC}
    ${CORE_SRC}/imgui
)

target_link_libraries(sdrpp_unit_tests PRIVATE catch2)

# Find volk (needed for stream.h / buffer.h)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VOLK REQUIRED volk)
pkg_check_modules(FFTW3F REQUIRED fftw3f)
target_include_directories(sdrpp_unit_tests PRIVATE ${VOLK_INCLUDE_DIRS} ${FFTW3F_INCLUDE_DIRS})
target_link_directories(sdrpp_unit_tests PRIVATE ${VOLK_LIBRARY_DIRS} ${FFTW3F_LIBRARY_DIRS})
target_link_libraries(sdrpp_unit_tests PRIVATE ${VOLK_LIBRARIES} ${FFTW3F_LIBRARIES})

enable_testing()
add_test(NAME unit_tests COMMAND sdrpp_unit_tests)
