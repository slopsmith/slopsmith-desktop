# Tests for the sandbox audio path. Win32-only because AudioChannel.cpp is.
# Built by default alongside the addon — opt-out with -DSLOPSMITH_BUILD_TESTS=OFF.
#
# Run from the build directory: ./Release/audio_channel_midi_test (exit 0 = pass).

if(NOT WIN32)
    return()
endif()

option(SLOPSMITH_BUILD_TESTS "Build sandbox unit tests" ON)
if(NOT SLOPSMITH_BUILD_TESTS)
    return()
endif()

add_subdirectory(sandbox)
