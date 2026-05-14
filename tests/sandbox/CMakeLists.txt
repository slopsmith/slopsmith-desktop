# audio_channel_midi_test — exercises the inline-MIDI path (pushInputBlock /
# popInputBlock + AudioShmHeader.midiOverflows) end-to-end without spawning a
# subprocess. Closes the v2/v3 review-thread concern about missing automated
# coverage for that code path.
#
# Standalone console exe; exits 0 on pass, 1 on fail.
#
# Dependency note: the JUCE targets (juce::juce_core, juce::juce_audio_basics)
# come from `add_subdirectory(JUCE)` at the top of the project's
# CMakeLists.txt, which runs BEFORE `add_subdirectory(tests)`. Guard against a
# future re-ordering that hides them — better to fail-fast at configure time
# than emit confusing link errors.
if(NOT TARGET juce::juce_core OR NOT TARGET juce::juce_audio_basics)
    message(FATAL_ERROR "audio_channel_midi_test: JUCE targets not in scope; "
                        "tests/CMakeLists.txt must run after JUCE is added.")
endif()

# Note: AudioChannel.cpp + Protocol.cpp are compiled directly into the test
# binary rather than linked from the addon's compiled library. This means any
# function-local statics inside AudioChannel.cpp (e.g. the one-shot warn
# flags in popBlock / popInputBlock) are SEPARATE copies from the production
# binary — fine for this isolated test but means tests can't share or assert
# against those one-shot warn states across the test/production boundary.
add_executable(audio_channel_midi_test
    audio_channel_midi_test.cpp
    ../../src/audio/Sandbox/AudioChannel.cpp
    ../../src/audio/Sandbox/Protocol.cpp
)

target_include_directories(audio_channel_midi_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/audio"
)

target_link_libraries(audio_channel_midi_test PRIVATE
    juce::juce_audio_basics
    juce::juce_core
)

target_compile_definitions(audio_channel_midi_test PRIVATE
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
)

# Drop into the same Release/ folder as slopsmith-vst-host so the smoke
# script can find it without juggling per-config paths.
set_target_properties(audio_channel_midi_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/Release"
    OUTPUT_NAME "audio_channel_midi_test"
)

# Register with CTest so `ctest --output-on-failure -C Release` from the
# build directory runs the test as part of the standard test suite. The
# enable_testing() call lives one level up in tests/CMakeLists.txt.
add_test(
    NAME audio_channel_midi_test
    COMMAND audio_channel_midi_test
)
