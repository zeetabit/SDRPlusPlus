cmake_minimum_required(VERSION 3.13)
project(kcsdr_source_v2_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Catch2 single-header (shared with main tests)
set(CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/catch2")
add_library(catch2_kcsdr INTERFACE)
target_include_directories(catch2_kcsdr INTERFACE ${CATCH2_DIR})

set(CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/src")
set(MOD_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(COMMON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/source_v2_common")

add_executable(kcsdr_source_v2_tests
    test_main.cpp
    test_v2_contract.cpp
    stubs.cpp
    kcsdr_stub.c
    # The module under test — its V2 contract is what we assert against.
    ${MOD_SRC}/main.cpp
    # Real core sources the tests need (kept minimal).
    ${CORE_SRC}/config.cpp
    ${CORE_SRC}/module_config.cpp
)

target_include_directories(kcsdr_source_v2_tests PRIVATE
    ${MOD_SRC}
    ${CORE_SRC}
    ${CORE_SRC}/imgui
    ${COMMON_SRC}
)

target_link_libraries(kcsdr_source_v2_tests PRIVATE catch2_kcsdr)

# volk for volk_16i_s32f_convert_32f in the worker (unreachable in T1-T3 but
# main.cpp references it so the linker needs the symbol). fftw3f for
# signal_path/iq_frontend.h which is pulled in transitively via signal_path.h.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VOLK REQUIRED volk)
pkg_check_modules(FFTW3F REQUIRED fftw3f)
target_include_directories(kcsdr_source_v2_tests PRIVATE
    ${VOLK_INCLUDE_DIRS}
    ${FFTW3F_INCLUDE_DIRS}
)
target_link_directories(kcsdr_source_v2_tests PRIVATE
    ${VOLK_LIBRARY_DIRS}
    ${FFTW3F_LIBRARY_DIRS}
)
target_link_libraries(kcsdr_source_v2_tests PRIVATE
    ${VOLK_LIBRARIES}
    ${FFTW3F_LIBRARIES}
)

enable_testing()
add_test(NAME kcsdr_source_v2_tests COMMAND kcsdr_source_v2_tests)
