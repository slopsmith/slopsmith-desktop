# Standalone native test for MlNoteDetector. Not part of the addon build —
# build it explicitly when you want to verify the ML detector end to end.
#
#   cmake -B build -DONNXRUNTIME_ROOT=/path/to/onnxruntime-<os-arch>-<ver>
#   cmake --build build
#   ./build/mlnd_test ../../resources/models/basic_pitch.onnx ../spike/test_guitar.wav

cmake_minimum_required(VERSION 3.22)
project(mlnd_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ONNXRUNTIME_ROOT)
    message(FATAL_ERROR "Set -DONNXRUNTIME_ROOT=/path/to/onnxruntime-<os>-<arch>-<ver>")
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${REPO_ROOT}/JUCE" juce_build EXCLUDE_FROM_ALL)

add_executable(mlnd_test
    test.cpp
    "${REPO_ROOT}/src/audio/MlNoteDetector.cpp")

target_include_directories(mlnd_test PRIVATE
    "${REPO_ROOT}/src/audio"
    "${ONNXRUNTIME_ROOT}/include")

target_compile_definitions(mlnd_test PRIVATE
    SLOPSMITH_ONNX_SUPPORT=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)

target_link_libraries(mlnd_test PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so")

set_target_properties(mlnd_test PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib")
