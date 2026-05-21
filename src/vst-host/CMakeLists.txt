# slopsmith-vst-host.exe — sandboxed VST3 host subprocess.
#
# Only built on Windows (constitution VIII; macOS / Linux sandbox PRs follow).
# Statically linked against JUCE so it can be relocated alongside the addon
# without dragging external runtime deps.

if(NOT WIN32)
    return()
endif()

set(VST_HOST_SOURCES
    main.cpp
    ../audio/VSTHost.cpp
    ../audio/Sandbox/Protocol.cpp
    ../audio/Sandbox/ControlChannel.cpp
    ../audio/Sandbox/AudioChannel.cpp
)

add_executable(slopsmith-vst-host WIN32 ${VST_HOST_SOURCES})

target_include_directories(slopsmith-vst-host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../audio"
)

target_link_libraries(slopsmith-vst-host PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
)

target_compile_definitions(slopsmith-vst-host PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_AU=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="SlopsmithVstHost"
    JUCE_APPLICATION_VERSION_STRING="0.1.0"
    JUCE_STANDALONE_APPLICATION=1
    JUCE_REPORT_APP_USAGE=0
)

if(MSVC)
    # /EHa for SEH-aware catch in VSTHost.cpp (matches src/audio/CMakeLists.txt).
    target_compile_options(slopsmith-vst-host PRIVATE /EHa)
    # Emit a PDB for Release builds so sandbox-child crash dumps can be
    # symbolised — see the matching block in src/audio/CMakeLists.txt for the
    # /Zi + /DEBUG:FULL + /OPT rationale.
    target_compile_options(slopsmith-vst-host PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(slopsmith-vst-host PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF>)
    # `add_executable(... WIN32 ...)` already implies /SUBSYSTEM:WINDOWS and
    # the matching entry point (WinMainCRTStartup, or wWinMainCRTStartup if
    # the toolchain ever picks up _UNICODE). No need to pin them explicitly.
endif()

# Drop the binary next to the addon for easy lookup by SandboxFactory_win.cpp.
# Use the per-config variant so the Visual Studio generator doesn't append
# a second $<CONFIG> suffix (which would yield build/Release/Release/...).
set_target_properties(slopsmith-vst-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/Release"
    OUTPUT_NAME "slopsmith-vst-host"
)
