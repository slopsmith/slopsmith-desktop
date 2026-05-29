# Tests for the sandbox audio path. Win32-only because AudioChannel.cpp is.
# Built by default alongside the addon — opt-out with -DSLOPSMITH_BUILD_TESTS=OFF.
#
# Run from the build directory: ./Release/audio_channel_midi_test (exit 0 = pass).

# Declare the option BEFORE the WIN32 guard so it shows up in cmake-gui /
# ccmake on non-Windows hosts too — discoverable, even if non-Windows builds
# always skip the actual targets.
option(SLOPSMITH_BUILD_TESTS "Build sandbox unit tests" ON)

# Pure-helper tests (no JUCE / no Win32 deps) build on every platform.
if(SLOPSMITH_BUILD_TESTS)
    add_subdirectory(audio_sanitize)
endif()

if(NOT WIN32)
    # The remaining sandbox tests are Win32-only (AudioChannel.cpp). Surface
    # the no-op so a user setting -DSLOPSMITH_BUILD_TESTS=ON on macOS/Linux
    # doesn't expect the sandbox targets (the cross-platform audio_sanitize
    # test above still built).
    if(SLOPSMITH_BUILD_TESTS)
        message(STATUS "Sandbox tests are Win32-only; only audio_sanitize "
                       "builds on this platform.")
    endif()
    return()
endif()

if(NOT SLOPSMITH_BUILD_TESTS)
    return()
endif()

# Note: enable_testing() lives in the top-level CMakeLists.txt — calling it
# only here would register tests in build/tests/CTestTestfile.cmake but
# `ctest` from the root build directory wouldn't discover them.

add_subdirectory(sandbox)
