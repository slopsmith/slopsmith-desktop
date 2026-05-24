cmake_minimum_required(VERSION 3.22)
project(slopsmith_audio VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# cmake-js integration
if(DEFINED CMAKE_JS_INC)
    message(STATUS "Building as Node.js native addon (cmake-js)")
else()
    message(WARNING "CMAKE_JS_INC not defined — building standalone (for testing only)")
endif()

# JUCE — add as subdirectory (submodule)
if(EXISTS "${CMAKE_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    message(FATAL_ERROR "JUCE submodule not found. Run: git submodule update --init --recursive")
endif()

# NAM Core (Neural Amp Modeler)
set(NAM_DIR "${CMAKE_SOURCE_DIR}/src/audio/third_party/NAM")
if(EXISTS "${NAM_DIR}/CMakeLists.txt")
    set(NAM_AVAILABLE ON)
    # NAM uses its own build system — we'll include its source directly
    message(STATUS "NeuralAmpModelerCore found")
else()
    set(NAM_AVAILABLE OFF)
    message(STATUS "NeuralAmpModelerCore not found — NAM support disabled")
endif()

# RTNeural
set(RTNEURAL_DIR "${CMAKE_SOURCE_DIR}/src/audio/third_party/RTNeural")
if(EXISTS "${RTNEURAL_DIR}/CMakeLists.txt")
    set(RTNEURAL_AVAILABLE ON)
    set(RTNEURAL_XSIMD ON CACHE BOOL "Use XSIMD backend for RTNeural")
    add_subdirectory("${RTNEURAL_DIR}" rtneural_build EXCLUDE_FROM_ALL)
    message(STATUS "RTNeural found")
else()
    set(RTNEURAL_AVAILABLE OFF)
    message(STATUS "RTNeural not found")
endif()

# ONNX Runtime — for the Basic Pitch ML note detector (MlNoteDetector).
# Sets ONNXRUNTIME_AVAILABLE; build still succeeds (YIN fallback) if it can't
# be obtained. See cmake/onnxruntime.cmake.
include("${CMAKE_SOURCE_DIR}/cmake/onnxruntime.cmake")

# SoundTouch — pitch-preserving tempo change for backing tracks (Phase 2).
set(SOUNDTOUCH_DIR "${CMAKE_SOURCE_DIR}/third_party/SoundTouch")
if(EXISTS "${SOUNDTOUCH_DIR}/CMakeLists.txt")
    set(SOUNDTOUCH_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${SOUNDTOUCH_DIR}" soundtouch_build EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${SOUNDTOUCH_BUILD_SHARED_LIBS})
    set(SOUNDTOUCH_AVAILABLE ON)
    message(STATUS "SoundTouch found — backing pitch-preserve stretch enabled")
else()
    set(SOUNDTOUCH_AVAILABLE OFF)
    message(STATUS "SoundTouch not found — backing uses resampling only")
endif()

add_subdirectory(src/audio)

# VST scan / sandbox subprocess helpers (Windows host, macOS scan helper).
if(WIN32 OR APPLE)
    add_subdirectory(src/vst-host)
endif()

# Tests (Win32-only; gated on SLOPSMITH_BUILD_TESTS, default ON).
# enable_testing() must be at the top level so `ctest` from the root build
# directory discovers tests registered via add_test() in subdirectories.
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
