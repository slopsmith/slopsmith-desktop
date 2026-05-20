# Slopsmith Audio Engine — Node.js native addon built with JUCE
# Produces slopsmith_audio.node loadable via require()

set(AUDIO_SOURCES
    NodeAddon.cpp
    NoiseGate.cpp
    TonePolish.cpp
    AudioEngine.cpp
    SignalChain.cpp
    VSTHost.cpp
    NAMProcessor.cpp
    IRLoader.cpp
    PitchDetector.cpp
    ChordScorer.cpp
    MlNoteDetector.cpp
    OnsetDetector.cpp
    NoteVerifier.cpp
    Sandbox/Protocol.cpp
)

# Plugin sandbox — out-of-process VST3 host for plugins that don't survive
# in-process loading (notably Qt5-using plugins from Native Instruments;
# see docs/VST-SANDBOX-DIAG.md for the diagnosis).
#
# Cross-platform note (constitution VIII): only the Windows implementation
# ships in this PR. macOS / Linux fall back to in-process loading via the
# stub factory; sandbox PRs for those OSes follow.
if(WIN32)
    list(APPEND AUDIO_SOURCES
        Sandbox/SandboxedProcessor.cpp
        Sandbox/ControlChannel.cpp
        Sandbox/AudioChannel.cpp
        Sandbox/SubprocessHandle.cpp
        Sandbox/SandboxFactory_win.cpp
    )
else()
    list(APPEND AUDIO_SOURCES
        Sandbox/SandboxFactory_stub.cpp
    )
endif()

# Build as shared library (.node)
add_library(slopsmith_audio SHARED ${AUDIO_SOURCES} ${CMAKE_JS_SRC})

# Output as .node file
set_target_properties(slopsmith_audio PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
)

# Node.js / N-API headers
if(DEFINED CMAKE_JS_INC)
    target_include_directories(slopsmith_audio PRIVATE ${CMAKE_JS_INC})
endif()

# node-addon-api headers (installed via npm)
execute_process(
    COMMAND node -p "require('node-addon-api').include"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE NAPI_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "\"" "" NAPI_INCLUDE_DIR "${NAPI_INCLUDE_DIR}")
target_include_directories(slopsmith_audio PRIVATE ${NAPI_INCLUDE_DIR})

# N-API version
target_compile_definitions(slopsmith_audio PRIVATE NAPI_VERSION=9 NAPI_DISABLE_CPP_EXCEPTIONS)

# Marks this translation unit set as the audio addon (vs. slopsmith-vst-host,
# which also compiles VSTHost.cpp). Gates the out-of-process VST scan path so
# it's only built into the addon — the host exe never calls scanDirectories.
target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_AUDIO_ADDON=1)

# JUCE modules — headless (no GUI)
target_link_libraries(slopsmith_audio PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_dsp
    juce::juce_events
)

# JUCE compile definitions
target_compile_definitions(slopsmith_audio PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="SlopsmithAudio"
    JUCE_STANDALONE_APPLICATION=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_LOG_ASSERTIONS=0
    JUCE_DISABLE_ASSERTIONS=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_FLAC=1
    # Backing tracks are commonly .mp3 (Rocksmith CDLC audio cache). Without
    # this, registerBasicFormats() omits MP3 and loadBackingTrack() rejects
    # every mp3, forcing the song onto the HTML5 path. JUCE's built-in MP3
    # decoder is patent-unencumbered and ships with the framework.
    JUCE_USE_MP3AUDIOFORMAT=1
)

# Platform-specific
if(APPLE)
    # NOTE: JUCE_PLUGINHOST_AU=1 requires linking CoreAudioKit/AppKit for
    # AUGenericView, but doing so causes JUCE's MessageManager to crash
    # when run from a Node.js native-addon thread (AppKit's NSRunLoop
    # expects the main thread). Disabling AU hosting on macOS until the
    # NodeAddon message-loop integration is redesigned. VST3 and NAM still
    # work; users can load VST3 plugins instead of AU.
    target_link_libraries(slopsmith_audio PRIVATE
        "-framework CoreAudio"
        "-framework CoreMIDI"
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework Accelerate"
    )
elseif(WIN32)
    target_compile_definitions(slopsmith_audio PRIVATE
        JUCE_ASIO=1
    )
    # Enable /EHa so catch(...) also catches SEH exceptions (ASIO driver crashes)
    if(MSVC)
        target_compile_options(slopsmith_audio PRIVATE /EHa)
    endif()
elseif(UNIX)
    # LV2 is Linux-only — on Windows, lilv probes every .vst3 bundle for a
    # manifest.ttl during scan and crashes the audio addon.
    target_compile_definitions(slopsmith_audio PRIVATE
        JUCE_PLUGINHOST_LV2=1
        JUCE_JACK=1
        JUCE_JACK_CLIENT_NAME="Slopsmith"
        JUCE_ALSA=1
    )
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    target_link_libraries(slopsmith_audio PRIVATE ${ALSA_LIBRARIES})
    target_include_directories(slopsmith_audio PRIVATE ${ALSA_INCLUDE_DIRS})

    # JACK is loaded dynamically by JUCE
    find_library(JACK_LIB jack)
    if(JACK_LIB)
        target_link_libraries(slopsmith_audio PRIVATE ${JACK_LIB})
    endif()
endif()

# cmake-js library
if(DEFINED CMAKE_JS_LIB)
    target_link_libraries(slopsmith_audio PRIVATE ${CMAKE_JS_LIB})
endif()

# NAM support
if(NAM_AVAILABLE)
    # Include NAM core sources directly
    file(GLOB NAM_SOURCES "${NAM_DIR}/NAM/*.cpp")
    target_sources(slopsmith_audio PRIVATE ${NAM_SOURCES})
    target_include_directories(slopsmith_audio PRIVATE
        "${NAM_DIR}"
        "${NAM_DIR}/Dependencies/eigen"
        "${NAM_DIR}/Dependencies/nlohmann"
        "${NAM_DIR}/Dependencies/AudioDSPTools"
    )
    target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_NAM_SUPPORT=1)

    if(RTNEURAL_AVAILABLE)
        target_link_libraries(slopsmith_audio PRIVATE RTNeural)
    endif()
else()
    target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_NAM_SUPPORT=0)
endif()

# ONNX Runtime — MlNoteDetector (Basic Pitch). When unavailable, the addon
# still builds and MlNoteDetector is compiled as an inert stub; the engine
# falls back to the YIN PitchDetector / ChordScorer (Constitution VII).
if(ONNXRUNTIME_AVAILABLE)
    target_include_directories(slopsmith_audio PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(slopsmith_audio PRIVATE "${ONNXRUNTIME_IMPORT_LIB}")
    target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_ONNX_SUPPORT=1)

    # The ONNX Runtime shared lib must load next to slopsmith_audio.node.
    # Linux: rpath $ORIGIN; macOS: @loader_path; Windows: DLL is searched in
    # the module's own directory by default.
    if(APPLE)
        set_target_properties(slopsmith_audio PROPERTIES
            BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(slopsmith_audio PROPERTIES
            BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()

    # Stage the runtime lib beside the .node so dev runs and electron-builder
    # packaging (asarUnpack of build/Release/*) both find it.
    add_custom_command(TARGET slopsmith_audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_RUNTIME_LIB}"
                "$<TARGET_FILE_DIR:slopsmith_audio>/"
        COMMENT "Staging ONNX Runtime shared library next to slopsmith_audio.node")

    # Also stage the shared-provider stub that ONNX Runtime dlopen()s next to
    # the main runtime. Conditional: it ships in the standard releases, but the
    # cmake probe leaves ONNXRUNTIME_PROVIDERS_LIB empty if a layout lacks it.
    if(ONNXRUNTIME_PROVIDERS_LIB)
        add_custom_command(TARGET slopsmith_audio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_PROVIDERS_LIB}"
                    "$<TARGET_FILE_DIR:slopsmith_audio>/"
            COMMENT "Staging ONNX Runtime providers_shared library next to slopsmith_audio.node")
    endif()
else()
    target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_ONNX_SUPPORT=0)
endif()

# Include directory for our headers
target_include_directories(slopsmith_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
