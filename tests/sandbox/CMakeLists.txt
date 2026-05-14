# audio_channel_midi_test — exercises the inline-MIDI path (pushInputBlock /
# popInputBlock + AudioShmHeader.midiOverflows) end-to-end without spawning a
# subprocess. Closes the v2/v3 review-thread concern about missing automated
# coverage for that code path.
#
# Standalone console exe; exits 0 on pass, 1 on fail.

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
