cmake_minimum_required(VERSION 3.13)
project(sdrpp)

# Backends
option(OPT_BACKEND_GLFW "Use the GLFW backend" ON)
option(OPT_BACKEND_ANDROID "Use the Android backend" OFF)

# Compatibility Options
option(OPT_OVERRIDE_STD_FILESYSTEM "Use a local version of std::filesystem on systems that don't have it yet" OFF)

# Sources
option(OPT_BUILD_AIRSPY_SOURCE "Build Airspy Source Module (Dependencies: libairspy)" ON)
option(OPT_BUILD_AIRSPYHF_SOURCE "Build Airspy HF+ Source Module (Dependencies: libairspyhf)" ON)
option(OPT_BUILD_AUDIO_SOURCE "Build Audio Source Module (Dependencies: rtaudio)" ON)
option(OPT_BUILD_BADGESDR_SOURCE "Build BadgeSDR Source Module (Dependencies: libusb)" OFF)
option(OPT_BUILD_BLADERF_SOURCE "Build BladeRF Source Module (Dependencies: libbladeRF)" OFF)
option(OPT_BUILD_DRAGONLABS_SOURCE "Build Dragon Labs Source Module (Dependencies: libdlcr)" OFF)
option(OPT_BUILD_FILE_SOURCE "Wav file source" ON)
option(OPT_BUILD_FOBOSSDR_SOURCE "Build FobosSDR Source Module (Dependencies: libfobos)" OFF)
option(OPT_BUILD_HACKRF_SOURCE "Build HackRF Source Module (Dependencies: libhackrf)" ON)
option(OPT_BUILD_HAROGIC_SOURCE "Build Harogic Source Module (Dependencies: htra_api)" OFF)
option(OPT_BUILD_HERMES_SOURCE "Build Hermes Source Module (no dependencies required)" ON)
option(OPT_BUILD_HYDRASDR_SOURCE "Build HydraSDR Source Module (Dependencies: libhydrasdr)" OFF)
option(OPT_BUILD_KCSDR_SOURCE "Build KCSDR Source Module (Dependencies: libkcsdr)" OFF)
option(OPT_BUILD_LIMESDR_SOURCE "Build LimeSDR Source Module (Dependencies: liblimesuite)" OFF)
option(OPT_BUILD_NETWORK_SOURCE "Build Network Source Module (no dependencies required)" ON)
option(OPT_BUILD_PERSEUS_SOURCE "Build Perseus Source Module (Dependencies: libperseus-sdr)" OFF)
option(OPT_BUILD_PLUTOSDR_SOURCE "Build PlutoSDR Source Module (Dependencies: libiio, libad9361)" ON)
option(OPT_BUILD_RFNM_SOURCE "Build RFNM Source Module (Dependencies: librfnm)" OFF)
option(OPT_BUILD_RFSPACE_SOURCE "Build RFspace Source Module (no dependencies required)" ON)
option(OPT_BUILD_RTL_SDR_SOURCE "Build RTL-SDR Source Module (Dependencies: librtlsdr)" ON)
option(OPT_BUILD_RTL_TCP_SOURCE "Build RTL-TCP Source Module (no dependencies required)" ON)
option(OPT_BUILD_SDRPP_SERVER_SOURCE "Build SDR++ Server Source Module (no dependencies required)" ON)
option(OPT_BUILD_SDRPLAY_SOURCE "Build SDRplay Source Module (Dependencies: libsdrplay)" OFF)
option(OPT_BUILD_SOAPY_SOURCE "Build SoapySDR Source Module (Dependencies: soapysdr)" OFF)
option(OPT_BUILD_SPECTRAN_SOURCE "Build Spectran Source Module (Dependencies: Aaronia RTSA Suite)" OFF)
option(OPT_BUILD_SPECTRAN_HTTP_SOURCE "Build Spectran HTTP Source Module (no dependencies required)" ON)
option(OPT_BUILD_SPYSERVER_SOURCE "Build SpyServer Source Module (no dependencies required)" ON)
option(OPT_BUILD_USRP_SOURCE "Build USRP Source Module (libuhd)" OFF)

# Sinks
option(OPT_BUILD_ANDROID_AUDIO_SINK "Build Android Audio Sink Module (Dependencies: AAudio, only for android)" OFF)
option(OPT_BUILD_AUDIO_SINK "Build Audio Sink Module (Dependencies: rtaudio)" ON)
option(OPT_BUILD_NETWORK_SINK "Build Audio Sink Module (no dependencies required)" ON)
option(OPT_BUILD_NEW_PORTAUDIO_SINK "Build the new PortAudio Sink Module (Dependencies: portaudio)" ON)
option(OPT_BUILD_PORTAUDIO_SINK "Build PortAudio Sink Module (Dependencies: portaudio)" OFF)

# Decoders
option(OPT_BUILD_ATV_DECODER "Build ATV decoder (no dependencies required)" ON)
option(OPT_BUILD_DAB_DECODER "Build the DAB/DAB+ decoder (no dependencies required)" OFF)
option(OPT_BUILD_FALCON9_DECODER "Build the falcon9 live decoder (Dependencies: ffplay)" OFF)
option(OPT_BUILD_KG_SSTV_DECODER "Build the KG SSTV (KG-STV) decoder module (no dependencies required)" OFF)
option(OPT_BUILD_M17_DECODER "Build the M17 decoder module (Dependencies: codec2)" OFF)
option(OPT_BUILD_METEOR_DEMODULATOR "Build the meteor demodulator module (no dependencies required)" ON)
option(OPT_BUILD_PAGER_DECODER "Build the pager decoder module (no dependencies required)" ON)
option(OPT_BUILD_RADIO "Main audio modulation decoder (AM, FM, SSB, etc...)" ON)
option(OPT_BUILD_RYFI_DECODER "RyFi data link decoder" OFF)
option(OPT_BUILD_VOR_RECEIVER "VOR beacon receiver" OFF)
option(OPT_BUILD_WEATHER_SAT_DECODER "Build the HRPT decoder module (no dependencies required)" OFF)
option(OPT_BUILD_CW_DECODER "Build the CW decoder module (no dependencies required)" ON)
option(OPT_BUILD_RADIOSONDE_DECODER "Build the radiosonde decoder module (no dependencies required)" ON)

# Misc
option(OPT_BUILD_DISCORD_PRESENCE "Build the Discord Rich Presence module" ON)
option(OPT_BUILD_FREQUENCY_MANAGER "Build the Frequency Manager module" ON)
option(OPT_BUILD_IQ_EXPORTER "Build the IQ Exporter module" ON)
option(OPT_BUILD_RECORDER "Audio and baseband recorder" ON)
option(OPT_BUILD_RIGCTL_CLIENT "Rigctl client to make SDR++ act as a panadapter" ON)
option(OPT_BUILD_RIGCTL_SERVER "Rigctl backend for controlling SDR++ with software like gpredict" ON)
option(OPT_BUILD_SCANNER "Frequency scanner" ON)
option(OPT_BUILD_SCHEDULER "Build the scheduler" ON)

# Other options
option(USE_INTERNAL_LIBCORRECT "Use an internal version of libcorrect" ON)
option(USE_BUNDLE_DEFAULTS "Set the default resource and module directories to the right ones for a MacOS .app" OFF)
option(COPY_MSVC_REDISTRIBUTABLES "Copy over the Visual C++ Redistributable" OFF)

# Module cmake path
set(SDRPP_MODULE_CMAKE "${CMAKE_SOURCE_DIR}/sdrpp_module.cmake")

# Root source folder
set(SDRPP_CORE_ROOT "${CMAKE_SOURCE_DIR}/core/src/")

# Compiler flags
if (${CMAKE_BUILD_TYPE} MATCHES "Debug")
    # Debug Flags
    if (MSVC)
        set(SDRPP_COMPILER_FLAGS /std:c++17 /EHsc)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_COMPILER_FLAGS -g -Og -std=c++17 -Wno-unused-command-line-argument -undefined dynamic_lookup)
    else ()
        set(SDRPP_COMPILER_FLAGS -g -Og -std=c++17)
    endif ()
else()
    # Normal Flags
    if (MSVC)
        set(SDRPP_COMPILER_FLAGS /O2 /Ob2 /std:c++17 /EHsc)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_COMPILER_FLAGS -O3 -std=c++17 -Wno-unused-command-line-argument -undefined dynamic_lookup)
    else ()
        set(SDRPP_COMPILER_FLAGS -O3 -std=c++17)
    endif ()
endif()
set(SDRPP_MODULE_COMPILER_FLAGS ${SDRPP_COMPILER_FLAGS})

# Set a default install prefix
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE PATH "..." FORCE)
    else()
        set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "..." FORCE)
    endif()
endif()

# Auto-detect available dependencies and disable modules that can't build.
# Uses a non-cached variable so CLion users can still override via CMake options.
# If you explicitly set -DOPT_BUILD_XXX=ON, auto-detection is skipped for that module.
option(OPT_AUTO_DETECT_DEPS "Auto-disable modules with missing dependencies" ON)

if (OPT_AUTO_DETECT_DEPS AND NOT ANDROID)
    find_package(PkgConfig QUIET)

    # Macro: check for a pkg-config library, auto-disable if not found.
    # Only acts when the option is still at its default ON state from this file.
    # If the user explicitly passed -DOPT_BUILD_XXX=ON, we don't override it.
    macro(check_pkg_and_disable pkg opt modname)
        if (${opt})
            if (PkgConfig_FOUND)
                pkg_check_modules(_${pkg}_CHK QUIET ${pkg})
            endif()
            if (NOT _${pkg}_CHK_FOUND)
                message(STATUS "[auto-detect] ${pkg} not found -- disabling ${modname}")
                set(${opt} OFF)
            endif()
        endif()
    endmacro()

    # macOS: PlutoSDR uses framework, not pkg-config
    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        if (OPT_BUILD_PLUTOSDR_SOURCE AND NOT EXISTS "/Library/Frameworks/iio.framework")
            message(STATUS "[auto-detect] iio.framework not found -- disabling plutosdr_source")
            set(OPT_BUILD_PLUTOSDR_SOURCE OFF)
        endif()
    else()
        check_pkg_and_disable(libiio OPT_BUILD_PLUTOSDR_SOURCE plutosdr_source)
    endif()

    check_pkg_and_disable(libairspy    OPT_BUILD_AIRSPY_SOURCE     airspy_source)
    check_pkg_and_disable(libairspyhf  OPT_BUILD_AIRSPYHF_SOURCE   airspyhf_source)
    check_pkg_and_disable(libhackrf    OPT_BUILD_HACKRF_SOURCE     hackrf_source)
    check_pkg_and_disable(librtlsdr    OPT_BUILD_RTL_SDR_SOURCE    rtl_sdr_source)
    check_pkg_and_disable(libsdrplay   OPT_BUILD_SDRPLAY_SOURCE    sdrplay_source)
    check_pkg_and_disable(SoapySDR     OPT_BUILD_SOAPY_SOURCE      soapy_source)
    check_pkg_and_disable(uhd          OPT_BUILD_USRP_SOURCE        usrp_source)
    check_pkg_and_disable(libbladeRF   OPT_BUILD_BLADERF_SOURCE    bladerf_source)
    check_pkg_and_disable(LimeSuite    OPT_BUILD_LIMESDR_SOURCE    limesdr_source)
    check_pkg_and_disable(libperseus-sdr OPT_BUILD_PERSEUS_SOURCE  perseus_source)

    # rtaudio is needed by audio_sink and audio_source
    check_pkg_and_disable(rtaudio       OPT_BUILD_AUDIO_SINK      audio_sink)
    check_pkg_and_disable(rtaudio       OPT_BUILD_AUDIO_SOURCE    audio_source)
endif()

# Configure toolchain for android
if (ANDROID)
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate"
    )
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX14_EXTENSION_COMPILE_OPTION "-std=c++17")
endif (ANDROID)

# Core of SDR++
add_subdirectory("core")

# Track all enabled module targets so sdrpp depends on them (needed for CLion/IDE builds)
set(SDRPP_MODULE_TARGETS "")
macro(add_sdrpp_module dir target)
    add_subdirectory(${dir})
    list(APPEND SDRPP_MODULE_TARGETS ${target})
endmacro()

# Source modules
if (OPT_BUILD_AIRSPY_SOURCE)
add_sdrpp_module("source_modules/airspy_source" airspy_source)
endif (OPT_BUILD_AIRSPY_SOURCE)

if (OPT_BUILD_AIRSPYHF_SOURCE)
add_sdrpp_module("source_modules/airspyhf_source" airspyhf_source)
endif (OPT_BUILD_AIRSPYHF_SOURCE)

if (OPT_BUILD_AUDIO_SOURCE)
add_sdrpp_module("source_modules/audio_source" audio_source)
endif (OPT_BUILD_AUDIO_SOURCE)

if (OPT_BUILD_BADGESDR_SOURCE)
add_sdrpp_module("source_modules/badgesdr_source" badgesdr_source)
endif (OPT_BUILD_BADGESDR_SOURCE)

if (OPT_BUILD_BLADERF_SOURCE)
add_sdrpp_module("source_modules/bladerf_source" bladerf_source)
endif (OPT_BUILD_BLADERF_SOURCE)

if (OPT_BUILD_DRAGONLABS_SOURCE)
add_sdrpp_module("source_modules/dragonlabs_source" dragonlabs_source)
endif (OPT_BUILD_DRAGONLABS_SOURCE)

if (OPT_BUILD_FILE_SOURCE)
add_sdrpp_module("source_modules/file_source" file_source)
endif (OPT_BUILD_FILE_SOURCE)

if (OPT_BUILD_FOBOSSDR_SOURCE)
add_sdrpp_module("source_modules/fobossdr_source" fobossdr_source)
endif (OPT_BUILD_FOBOSSDR_SOURCE)

if (OPT_BUILD_HACKRF_SOURCE)
add_sdrpp_module("source_modules/hackrf_source" hackrf_source)
endif (OPT_BUILD_HACKRF_SOURCE)

if (OPT_BUILD_HAROGIC_SOURCE)
add_sdrpp_module("source_modules/harogic_source" harogic_source)
endif (OPT_BUILD_HAROGIC_SOURCE)

if (OPT_BUILD_HERMES_SOURCE)
add_sdrpp_module("source_modules/hermes_source" hermes_source)
endif (OPT_BUILD_HERMES_SOURCE)

if (OPT_BUILD_HYDRASDR_SOURCE)
add_sdrpp_module("source_modules/hydrasdr_source" hydrasdr_source)
endif (OPT_BUILD_HYDRASDR_SOURCE)

if (OPT_BUILD_KCSDR_SOURCE)
add_sdrpp_module("source_modules/kcsdr_source" kcsdr_source)
endif (OPT_BUILD_KCSDR_SOURCE)

if (OPT_BUILD_LIMESDR_SOURCE)
add_sdrpp_module("source_modules/limesdr_source" limesdr_source)
endif (OPT_BUILD_LIMESDR_SOURCE)

if (OPT_BUILD_NETWORK_SOURCE)
add_sdrpp_module("source_modules/network_source" network_source)
endif (OPT_BUILD_NETWORK_SOURCE)

if (OPT_BUILD_PERSEUS_SOURCE)
add_sdrpp_module("source_modules/perseus_source" perseus_source)
endif (OPT_BUILD_PERSEUS_SOURCE)

if (OPT_BUILD_PLUTOSDR_SOURCE)
add_sdrpp_module("source_modules/plutosdr_source" plutosdr_source)
endif (OPT_BUILD_PLUTOSDR_SOURCE)

if (OPT_BUILD_RFNM_SOURCE)
add_sdrpp_module("source_modules/rfnm_source" rfnm_source)
endif (OPT_BUILD_RFNM_SOURCE)

if (OPT_BUILD_RFSPACE_SOURCE)
add_sdrpp_module("source_modules/rfspace_source" rfspace_source)
endif (OPT_BUILD_RFSPACE_SOURCE)

if (OPT_BUILD_RTL_SDR_SOURCE)
add_sdrpp_module("source_modules/rtl_sdr_source" rtl_sdr_source)
endif (OPT_BUILD_RTL_SDR_SOURCE)

if (OPT_BUILD_RTL_TCP_SOURCE)
add_sdrpp_module("source_modules/rtl_tcp_source" rtl_tcp_source)
endif (OPT_BUILD_RTL_TCP_SOURCE)

if (OPT_BUILD_SDDC_SOURCE)
add_sdrpp_module("source_modules/sddc_source" sddc_source)
endif (OPT_BUILD_SDDC_SOURCE)

if (OPT_BUILD_SDRPP_SERVER_SOURCE)
add_sdrpp_module("source_modules/sdrpp_server_source" sdrpp_server_source)
endif (OPT_BUILD_SDRPP_SERVER_SOURCE)

if (OPT_BUILD_SDRPLAY_SOURCE)
add_sdrpp_module("source_modules/sdrplay_source" sdrplay_source)
endif (OPT_BUILD_SDRPLAY_SOURCE)

if (OPT_BUILD_SOAPY_SOURCE)
add_sdrpp_module("source_modules/soapy_source" soapy_source)
endif (OPT_BUILD_SOAPY_SOURCE)

if (OPT_BUILD_SPECTRAN_SOURCE)
add_sdrpp_module("source_modules/spectran_source" spectran_source)
endif (OPT_BUILD_SPECTRAN_SOURCE)

if (OPT_BUILD_SPECTRAN_HTTP_SOURCE)
add_sdrpp_module("source_modules/spectran_http_source" spectran_http_source)
endif (OPT_BUILD_SPECTRAN_HTTP_SOURCE)

if (OPT_BUILD_SPYSERVER_SOURCE)
add_sdrpp_module("source_modules/spyserver_source" spyserver_source)
endif (OPT_BUILD_SPYSERVER_SOURCE)

if (OPT_BUILD_USRP_SOURCE)
add_sdrpp_module("source_modules/usrp_source" usrp_source)
endif (OPT_BUILD_USRP_SOURCE)


# Sink modules
if (OPT_BUILD_ANDROID_AUDIO_SINK)
add_sdrpp_module("sink_modules/android_audio_sink" android_audio_sink)
endif (OPT_BUILD_ANDROID_AUDIO_SINK)

if (OPT_BUILD_AUDIO_SINK)
add_sdrpp_module("sink_modules/audio_sink" audio_sink)
endif (OPT_BUILD_AUDIO_SINK)

if (OPT_BUILD_PORTAUDIO_SINK)
add_sdrpp_module("sink_modules/portaudio_sink" portaudio_sink)
endif (OPT_BUILD_PORTAUDIO_SINK)

if (OPT_BUILD_NETWORK_SINK)
add_sdrpp_module("sink_modules/network_sink" network_sink)
endif (OPT_BUILD_NETWORK_SINK)

if (OPT_BUILD_NEW_PORTAUDIO_SINK)
add_sdrpp_module("sink_modules/new_portaudio_sink" new_portaudio_sink)
endif (OPT_BUILD_NEW_PORTAUDIO_SINK)


# Decoders
if (OPT_BUILD_ATV_DECODER)
add_sdrpp_module("decoder_modules/atv_decoder" atv_decoder)
endif (OPT_BUILD_ATV_DECODER)

if (OPT_BUILD_DAB_DECODER)
add_sdrpp_module("decoder_modules/dab_decoder" dab_decoder)
endif (OPT_BUILD_DAB_DECODER)

if (OPT_BUILD_FALCON9_DECODER)
add_sdrpp_module("decoder_modules/falcon9_decoder" falcon9_decoder)
endif (OPT_BUILD_FALCON9_DECODER)

if (OPT_BUILD_KG_SSTV_DECODER)
add_sdrpp_module("decoder_modules/kg_sstv_decoder" kg_sstv_decoder)
endif (OPT_BUILD_KG_SSTV_DECODER)

if (OPT_BUILD_M17_DECODER)
add_sdrpp_module("decoder_modules/m17_decoder" m17_decoder)
endif (OPT_BUILD_M17_DECODER)

if (OPT_BUILD_METEOR_DEMODULATOR)
add_sdrpp_module("decoder_modules/meteor_demodulator" meteor_demodulator)
endif (OPT_BUILD_METEOR_DEMODULATOR)

if (OPT_BUILD_PAGER_DECODER)
add_sdrpp_module("decoder_modules/pager_decoder" pager_decoder)
endif (OPT_BUILD_PAGER_DECODER)

if (OPT_BUILD_RADIO)
add_sdrpp_module("decoder_modules/radio" radio)
endif (OPT_BUILD_RADIO)

if (OPT_BUILD_RYFI_DECODER)
add_sdrpp_module("decoder_modules/ryfi_decoder" ryfi_decoder)
endif (OPT_BUILD_RYFI_DECODER)

if (OPT_BUILD_VOR_RECEIVER)
add_sdrpp_module("decoder_modules/vor_receiver" vor_receiver)
endif (OPT_BUILD_VOR_RECEIVER)

if (OPT_BUILD_WEATHER_SAT_DECODER)
add_sdrpp_module("decoder_modules/weather_sat_decoder" weather_sat_decoder)
endif (OPT_BUILD_WEATHER_SAT_DECODER)

if (OPT_BUILD_CW_DECODER)
add_sdrpp_module("decoder_modules/cw_decoder" cw_decoder)
endif (OPT_BUILD_CW_DECODER)

if (OPT_BUILD_RADIOSONDE_DECODER)
add_subdirectory("decoder_modules/radiosonde_decoder")
list(APPEND SDRPP_MODULE_TARGETS radiosonde_decoder)
endif (OPT_BUILD_RADIOSONDE_DECODER)


# Misc
if (OPT_BUILD_DISCORD_PRESENCE)
add_sdrpp_module("misc_modules/discord_integration" discord_integration)
endif (OPT_BUILD_DISCORD_PRESENCE)

if (OPT_BUILD_FREQUENCY_MANAGER)
add_sdrpp_module("misc_modules/frequency_manager" frequency_manager)
endif (OPT_BUILD_FREQUENCY_MANAGER)

if (OPT_BUILD_IQ_EXPORTER)
add_sdrpp_module("misc_modules/iq_exporter" iq_exporter)
endif (OPT_BUILD_IQ_EXPORTER)

if (OPT_BUILD_RECORDER)
add_sdrpp_module("misc_modules/recorder" recorder)
endif (OPT_BUILD_RECORDER)

if (OPT_BUILD_RIGCTL_CLIENT)
add_sdrpp_module("misc_modules/rigctl_client" rigctl_client)
endif (OPT_BUILD_RIGCTL_CLIENT)

if (OPT_BUILD_RIGCTL_SERVER)
add_sdrpp_module("misc_modules/rigctl_server" rigctl_server)
endif (OPT_BUILD_RIGCTL_SERVER)

if (OPT_BUILD_SCANNER)
add_sdrpp_module("misc_modules/scanner" scanner)
endif (OPT_BUILD_SCANNER)

if (OPT_BUILD_SCHEDULER)
add_sdrpp_module("misc_modules/scheduler" scheduler)
endif (OPT_BUILD_SCHEDULER)

if (MSVC)
    add_executable(sdrpp "src/main.cpp" "win32/resources.rc")
else ()
    add_executable(sdrpp "src/main.cpp")
endif ()

target_link_libraries(sdrpp PRIVATE sdrpp_core)

# Compiler arguments
target_compile_options(sdrpp PRIVATE ${SDRPP_COMPILER_FLAGS})

# Make sdrpp depend on all enabled modules so IDEs build everything
if (SDRPP_MODULE_TARGETS)
    add_dependencies(sdrpp ${SDRPP_MODULE_TARGETS})
endif()

# Copy dynamic libs over
if (MSVC)
    add_custom_target(do_always ALL xcopy /s \"$<TARGET_FILE_DIR:sdrpp_core>\\*.dll\" \"$<TARGET_FILE_DIR:sdrpp>\" /Y)
    add_custom_target(do_always_volk ALL xcopy /s \"C:/Program Files/PothosSDR/bin\\volk.dll\" \"$<TARGET_FILE_DIR:sdrpp>\" /Y)

    if (COPY_MSVC_REDISTRIBUTABLES)
        # Get the list of Visual C++ runtime DLLs
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP True)
        include(InstallRequiredSystemLibraries)

        # Create a space sperated list
        set(REDIST_DLLS_STR "")
        foreach(DLL IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            set(REDIST_DLLS_STR COMMAND xcopy /F \"${DLL}\" \"$<TARGET_FILE_DIR:sdrpp>\" /Y ${REDIST_DLLS_STR})
        endforeach()
        
        # Create target
        add_custom_target(do_always_msvc ALL ${REDIST_DLLS_STR})
    endif ()
endif ()


if (${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.so\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    target_link_libraries(sdrpp PUBLIC pthread)
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.so\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.so\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.dylib\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

# cmake .. "-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake" -DOPT_BUILD_BLADERF_SOURCE=ON -DOPT_BUILD_LIMESDR_SOURCE=ON -DOPT_BUILD_SDRPLAY_SOURCE=ON -DOPT_BUILD_M17_DECODER=ON -DOPT_BUILD_SCANNER=ON -DOPT_BUILD_SCHEDULER=ON -DOPT_BUILD_USRP_SOURCE=ON -DOPT_BUILD_PAGER_DECODER=ON

# Create module cmake file
configure_file(${CMAKE_SOURCE_DIR}/sdrpp_module.cmake ${CMAKE_CURRENT_BINARY_DIR}/sdrpp_module.cmake @ONLY)

# ===== Configuration Summary =====
function(print_module_status name option)
    if (${option})
        list(APPEND SDRPP_ENABLED_MODULES "${name}")
        set(SDRPP_ENABLED_MODULES ${SDRPP_ENABLED_MODULES} PARENT_SCOPE)
    else()
        list(APPEND SDRPP_DISABLED_MODULES "${name}")
        set(SDRPP_DISABLED_MODULES ${SDRPP_DISABLED_MODULES} PARENT_SCOPE)
    endif()
endfunction()

set(SDRPP_ENABLED_MODULES "")
set(SDRPP_DISABLED_MODULES "")

# Sources
print_module_status("airspy_source"         OPT_BUILD_AIRSPY_SOURCE)
print_module_status("airspyhf_source"       OPT_BUILD_AIRSPYHF_SOURCE)
print_module_status("audio_source"          OPT_BUILD_AUDIO_SOURCE)
print_module_status("badgesdr_source"       OPT_BUILD_BADGESDR_SOURCE)
print_module_status("bladerf_source"        OPT_BUILD_BLADERF_SOURCE)
print_module_status("dragonlabs_source"     OPT_BUILD_DRAGONLABS_SOURCE)
print_module_status("file_source"           OPT_BUILD_FILE_SOURCE)
print_module_status("fobossdr_source"       OPT_BUILD_FOBOSSDR_SOURCE)
print_module_status("hackrf_source"         OPT_BUILD_HACKRF_SOURCE)
print_module_status("harogic_source"        OPT_BUILD_HAROGIC_SOURCE)
print_module_status("hermes_source"         OPT_BUILD_HERMES_SOURCE)
print_module_status("hydrasdr_source"       OPT_BUILD_HYDRASDR_SOURCE)
print_module_status("kcsdr_source"          OPT_BUILD_KCSDR_SOURCE)
print_module_status("limesdr_source"        OPT_BUILD_LIMESDR_SOURCE)
print_module_status("network_source"        OPT_BUILD_NETWORK_SOURCE)
print_module_status("perseus_source"        OPT_BUILD_PERSEUS_SOURCE)
print_module_status("plutosdr_source"       OPT_BUILD_PLUTOSDR_SOURCE)
print_module_status("rfnm_source"           OPT_BUILD_RFNM_SOURCE)
print_module_status("rfspace_source"        OPT_BUILD_RFSPACE_SOURCE)
print_module_status("rtl_sdr_source"        OPT_BUILD_RTL_SDR_SOURCE)
print_module_status("rtl_tcp_source"        OPT_BUILD_RTL_TCP_SOURCE)
print_module_status("sddc_source"           OPT_BUILD_SDDC_SOURCE)
print_module_status("sdrpp_server_source"   OPT_BUILD_SDRPP_SERVER_SOURCE)
print_module_status("sdrplay_source"        OPT_BUILD_SDRPLAY_SOURCE)
print_module_status("soapy_source"          OPT_BUILD_SOAPY_SOURCE)
print_module_status("spectran_source"       OPT_BUILD_SPECTRAN_SOURCE)
print_module_status("spectran_http_source"  OPT_BUILD_SPECTRAN_HTTP_SOURCE)
print_module_status("spyserver_source"      OPT_BUILD_SPYSERVER_SOURCE)
print_module_status("usrp_source"           OPT_BUILD_USRP_SOURCE)
# Sinks
print_module_status("android_audio_sink"    OPT_BUILD_ANDROID_AUDIO_SINK)
print_module_status("audio_sink"            OPT_BUILD_AUDIO_SINK)
print_module_status("network_sink"          OPT_BUILD_NETWORK_SINK)
print_module_status("new_portaudio_sink"    OPT_BUILD_NEW_PORTAUDIO_SINK)
print_module_status("portaudio_sink"        OPT_BUILD_PORTAUDIO_SINK)
# Decoders
print_module_status("atv_decoder"           OPT_BUILD_ATV_DECODER)
print_module_status("dab_decoder"           OPT_BUILD_DAB_DECODER)
print_module_status("falcon9_decoder"       OPT_BUILD_FALCON9_DECODER)
print_module_status("kg_sstv_decoder"       OPT_BUILD_KG_SSTV_DECODER)
print_module_status("m17_decoder"           OPT_BUILD_M17_DECODER)
print_module_status("meteor_demodulator"    OPT_BUILD_METEOR_DEMODULATOR)
print_module_status("pager_decoder"         OPT_BUILD_PAGER_DECODER)
print_module_status("radio"                 OPT_BUILD_RADIO)
print_module_status("ryfi_decoder"          OPT_BUILD_RYFI_DECODER)
print_module_status("vor_receiver"          OPT_BUILD_VOR_RECEIVER)
print_module_status("weather_sat_decoder"   OPT_BUILD_WEATHER_SAT_DECODER)
print_module_status("cw_decoder"            OPT_BUILD_CW_DECODER)
print_module_status("radiosonde_decoder"    OPT_BUILD_RADIOSONDE_DECODER)
# Misc
print_module_status("discord_integration"   OPT_BUILD_DISCORD_PRESENCE)
print_module_status("frequency_manager"     OPT_BUILD_FREQUENCY_MANAGER)
print_module_status("iq_exporter"           OPT_BUILD_IQ_EXPORTER)
print_module_status("recorder"              OPT_BUILD_RECORDER)
print_module_status("rigctl_client"         OPT_BUILD_RIGCTL_CLIENT)
print_module_status("rigctl_server"         OPT_BUILD_RIGCTL_SERVER)
print_module_status("scanner"               OPT_BUILD_SCANNER)
print_module_status("scheduler"             OPT_BUILD_SCHEDULER)

list(LENGTH SDRPP_ENABLED_MODULES _enabled_count)
list(LENGTH SDRPP_DISABLED_MODULES _disabled_count)

message(STATUS "")
message(STATUS "=========================================")
message(STATUS "  SDR++ Configuration Summary")
message(STATUS "=========================================")
# Read version from version.h (single source of truth)
file(STRINGS "${CMAKE_SOURCE_DIR}/core/src/version.h" _ver_line REGEX "VERSION_STR")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_ver_line}")
set(SDRPP_VERSION "${CMAKE_MATCH_1}")
message(STATUS "  Version:       ${SDRPP_VERSION}")
message(STATUS "  Platform:      ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Compiler:      ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install prefix:${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "  Modules enabled:  ${_enabled_count}")
message(STATUS "  Modules disabled: ${_disabled_count}")
message(STATUS "")
if (SDRPP_ENABLED_MODULES)
    string(REPLACE ";" ", " _enabled_str "${SDRPP_ENABLED_MODULES}")
    message(STATUS "  [ON]  ${_enabled_str}")
endif()
if (SDRPP_DISABLED_MODULES)
    string(REPLACE ";" ", " _disabled_str "${SDRPP_DISABLED_MODULES}")
    message(STATUS "  [OFF] ${_disabled_str}")
endif()
message(STATUS "")
message(STATUS "  Backend:       ${OPT_BACKEND_GLFW} (GLFW) / ${OPT_BACKEND_ANDROID} (Android)")
message(STATUS "  Internal libcorrect: ${USE_INTERNAL_LIBCORRECT}")
message(STATUS "=========================================")

# Install directives
install(TARGETS sdrpp DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/bandplans DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/colormaps DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/fonts DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/icons DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/themes DESTINATION share/sdrpp)
configure_file(${CMAKE_SOURCE_DIR}/sdrpp.desktop ${CMAKE_CURRENT_BINARY_DIR}/sdrpp.desktop @ONLY)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sdrpp.desktop DESTINATION share/applications)
endif ()

# Create uninstall target
configure_file(${CMAKE_SOURCE_DIR}/cmake_uninstall.cmake ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

# Create headers target
