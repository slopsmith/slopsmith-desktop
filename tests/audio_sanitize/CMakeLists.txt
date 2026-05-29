# Pure, JUCE-free unit test for the #403 audio-output containment helper
# (src/audio/AudioSanitize.h). Cross-platform — unlike the Win32-only sandbox
# tests, this builds and runs on Linux/macOS CI too.
add_executable(audio_sanitize_test test.cpp)
target_compile_features(audio_sanitize_test PRIVATE cxx_std_17)
add_test(NAME audio_sanitize COMMAND audio_sanitize_test)
