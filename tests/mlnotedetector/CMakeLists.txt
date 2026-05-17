# Standalone native test + benchmark for MlNoteDetector. Not part of the
# addon build — build explicitly to verify / tune the ML detector.
#
#   cmake -B build -DONNXRUNTIME_ROOT=/path/to/onnxruntime-<os-arch>-<ver>
#   cmake --build build
#   ./build/mlnd_test  ../../resources/models/basic_pitch.onnx ../spike/test_guitar.wav
#   ./build/mlnd_bench ../../resources/models/basic_pitch.onnx <di-take.wav> <chart.txt>
#
# mlnd_bench replays a fixed DI recording against a known chart so detector
# parameter changes can be measured (recall / timing) instead of guessed.

cmake_minimum_required(VERSION 3.22)
project(mlnd_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ONNXRUNTIME_ROOT)
    message(FATAL_ERROR "Set -DONNXRUNTIME_ROOT=/path/to/onnxruntime-<os>-<arch>-<ver>")
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${REPO_ROOT}/JUCE" juce_build EXCLUDE_FROM_ALL)

# ONNX Runtime ships a platform-specific library file name — resolve it so
# the documented ONNXRUNTIME_ROOT build works on macOS and Windows too.
if(WIN32)
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
elseif(APPLE)
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.dylib")
else()
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so")
endif()

foreach(tgt mlnd_test mlnd_bench)
    if(tgt STREQUAL mlnd_test)
        set(_src test.cpp)
    else()
        set(_src bench.cpp)
    endif()
    add_executable(${tgt} ${_src} "${REPO_ROOT}/src/audio/MlNoteDetector.cpp")
    target_include_directories(${tgt} PRIVATE
        "${REPO_ROOT}/src/audio"
        "${ONNXRUNTIME_ROOT}/include")
    target_compile_definitions(${tgt} PRIVATE
        SLOPSMITH_ONNX_SUPPORT=1
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(${tgt} PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        "${ONNXRUNTIME_LIB}")
    set_target_properties(${tgt} PROPERTIES
        BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib")
    # Windows ignores RPATH — copy the ONNX Runtime DLL next to the test
    # executable so the documented `./build/mlnd_test ...` run can load it.
    if(WIN32)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                "$<TARGET_FILE_DIR:${tgt}>")
    endif()
endforeach()
