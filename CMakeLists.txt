cmake_minimum_required(VERSION 3.25)
project(sydrahynth VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The plugin is a shared library, so everything linked into it must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(CTest)
find_package(Threads REQUIRED)
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
function(hydra_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/boost/boost/version.hpp")
    set(BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/boost")
    set(Boost_NO_BOOST_CMAKE ON)
endif()

find_package(Boost 1.57 REQUIRED)

# Dynarmic (vendored, patched: see patches/). Only the A32 frontend is needed.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

add_library(hydra_core STATIC
    src/a5_peripherals.cpp
    src/a5pack.cpp
    src/bus.cpp
    src/arm_system.cpp
    src/cp15.cpp
    src/cortex_m3.cpp
    src/cpu.cpp
    src/dsp_block.cpp
    src/dsp_core.cpp
    src/dsp_jit.cpp
    src/effects_rack.cpp
    src/emulator.cpp
    src/fx_dsp.cpp
    src/i2c.cpp
    src/machine.cpp
    src/nor_flash.cpp
    src/front_panel.cpp
    src/oled.cpp
    src/panel_mcu.cpp
    src/patch_bank.cpp
    src/spi.cpp
    src/stm32.cpp
    src/sysex.cpp
    src/tone_generator.cpp
    src/wave_rom.cpp
)
target_include_directories(hydra_core PUBLIC src)
target_link_libraries(hydra_core PUBLIC dynarmic merry::mcl Boost::boost Threads::Threads PRIVATE xbyak::xbyak)
# Same Xbyak configuration as Dynarmic: its inline functions are shared (ODR).
target_compile_definitions(hydra_core PRIVATE XBYAK_STRICT_CHECK_MEM_REG_SIZE=0)
hydra_warnings(hydra_core)

add_executable(hydraboot tools/hydraboot.cpp)
target_link_libraries(hydraboot PRIVATE hydra_core)
hydra_warnings(hydraboot)

add_executable(armdis tools/armdis.cpp)
target_link_libraries(armdis PRIVATE hydra_core)

add_executable(dsptest tools/dsptest.cpp)
target_link_libraries(dsptest PRIVATE hydra_core)
hydra_warnings(dsptest)

add_executable(waverom tools/waverom.cpp)
target_link_libraries(waverom PRIVATE hydra_core)

add_executable(panelboot tools/panelboot.cpp)
target_link_libraries(panelboot PRIVATE hydra_core)
hydra_warnings(panelboot)

add_executable(emurun tools/emurun.cpp)
target_link_libraries(emurun PRIVATE hydra_core)
hydra_warnings(emurun)

add_executable(firmware_smoke tools/firmware_smoke.cpp)
target_link_libraries(firmware_smoke PRIVATE hydra_core)
hydra_warnings(firmware_smoke)

add_executable(firmware_perf tools/firmware_perf.cpp)
target_link_libraries(firmware_perf PRIVATE hydra_core)
hydra_warnings(firmware_perf)

add_executable(dsp_sync_bench tools/dsp_sync_bench.cpp)
target_link_libraries(dsp_sync_bench PRIVATE hydra_core)

# The plugin (needs JUCE in external/JUCE: scripts/fetch-deps.sh).
option(HYDRA_BUILD_PLUGIN "Build the VST3/LV2/standalone plugin" ON)
if(HYDRA_BUILD_PLUGIN AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "JUCE is missing; run scripts/fetch-deps.py or set HYDRA_BUILD_PLUGIN=OFF")
endif()
if (HYDRA_BUILD_PLUGIN)
    set(HYDRA_PLUGIN_FORMATS VST3 Standalone)
    if(UNIX AND NOT APPLE)
        list(APPEND HYDRA_PLUGIN_FORMATS LV2)
    endif()
    # JUCE's manifest helper executes target code during the build. VST3 hosts
    # can enumerate directly; omit this optional cache on a non-Windows host.
    set(HYDRA_VST3_AUTO_MANIFEST TRUE)
    if(CMAKE_CROSSCOMPILING AND WIN32 AND NOT CMAKE_HOST_WIN32)
        set(HYDRA_VST3_AUTO_MANIFEST FALSE)
    endif()
    add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)
    juce_add_plugin(Sydrahynth
        PRODUCT_NAME "Sydrahynth"
        VST3_AUTO_MANIFEST ${HYDRA_VST3_AUTO_MANIFEST}
        COMPANY_NAME "sydrahynth"
        PLUGIN_MANUFACTURER_CODE Sydr
        PLUGIN_CODE Sdhy
        FORMATS ${HYDRA_PLUGIN_FORMATS}
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        VST3_CATEGORIES Instrument Synth
        LV2URI "urn:sydrahynth:plugin"
        COPY_PLUGIN_AFTER_BUILD FALSE)
    target_sources(Sydrahynth PRIVATE
        plugin/HydraProcessor.cpp
        plugin/HydraEditor.cpp
        plugin/PanelView.cpp
        plugin/PatchBrowser.cpp)
    target_include_directories(Sydrahynth PRIVATE plugin)
    target_compile_definitions(Sydrahynth PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(Sydrahynth
        PRIVATE hydra_core juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags)
    if(MINGW)
        # GNU-style Windows linkers do not honor MSVC's pragma comment(lib).
        set(HYDRA_WIN_LIBS ws2_32 wininet version shlwapi winmm dbghelp
            vfw32 imm32 comctl32 dwmapi shcore dwrite d2d1 dxgi d3d11 dcomp
            dxguid dsound ksuser sapi ole32 oleaut32 uuid shell32 comdlg32 gdi32 advapi32)
        target_link_libraries(Sydrahynth PRIVATE ${HYDRA_WIN_LIBS})
    endif()
    hydra_warnings(Sydrahynth)

    # Headless host that loads the built VST3 (tools/plugintest.cpp).
    juce_add_console_app(plugintest PRODUCT_NAME "plugintest")
    target_sources(plugintest PRIVATE tools/plugintest.cpp)
    target_compile_definitions(plugintest PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(plugintest PRIVATE hydra_core juce::juce_audio_processors juce::juce_audio_formats
        juce::juce_recommended_config_flags)
    if(MINGW)
        target_link_libraries(plugintest PRIVATE ${HYDRA_WIN_LIBS})
    endif()
endif()

# Exercises the generated-code ABI and exact portable DSP arithmetic.
if(BUILD_TESTING)
    add_executable(dsp_workers_test tests/dsp_workers_test.cpp)
    target_link_libraries(dsp_workers_test PRIVATE hydra_core)
    add_test(NAME dsp_workers_test COMMAND dsp_workers_test)
    set_tests_properties(dsp_workers_test PROPERTIES TIMEOUT 60)
    add_executable(dsp_port_test tests/dsp_port_test.cpp)
    target_link_libraries(dsp_port_test PRIVATE hydra_core)
    add_test(NAME dsp_port_test COMMAND dsp_port_test)
    if(NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_executable(dsp_windows_abi_test tests/dsp_port_test.cpp src/dsp_core.cpp src/dsp_jit.cpp)
        target_include_directories(dsp_windows_abi_test PRIVATE src)
        target_link_libraries(dsp_windows_abi_test PRIVATE xbyak::xbyak Threads::Threads)
        target_compile_definitions(dsp_windows_abi_test PRIVATE HYDRA_TEST_WINDOWS_ABI HYDRA_PORTABLE_DISPATCH XBYAK_STRICT_CHECK_MEM_REG_SIZE=0)
        add_test(NAME dsp_windows_abi_test COMMAND dsp_windows_abi_test)
    endif()
endif()

# JUCE uses MSVC linker pragmas for Common Controls v6. GNU-style linkers
# ignore those pragmas, so embed the equivalent activation context explicitly.
# EXEs use resource ID 1; DLLs use ID 2 (ISOLATIONAWARE_MANIFEST_RESOURCE_ID).
if(MINGW AND HYDRA_BUILD_PLUGIN)
    function(hydra_embed_manifest target resource_id filename)
        set(HYDRA_MANIFEST_ID "${resource_id}")
        set(HYDRA_MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/${filename}")
        set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_manifest.rc")
        configure_file(resources/windows/manifest.rc.in "${rc}" @ONLY)
        set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${HYDRA_MANIFEST_FILE}")
        target_sources(${target} PRIVATE "${rc}")
    endfunction()
    hydra_embed_manifest(Sydrahynth_Standalone 1 application.manifest)
    hydra_embed_manifest(plugintest 1 application.manifest)
    hydra_embed_manifest(Sydrahynth_VST3 2 common-controls.manifest)
endif()
