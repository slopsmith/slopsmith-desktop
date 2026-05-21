# Phase 0 de-risk spike — standalone, NOT part of the addon build.
#
# Build:
#   cmake -B build -DONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-1.20.1
#   cmake --build build
#
# ONNXRUNTIME_ROOT must point at an extracted prebuilt ONNX Runtime release
# (the dir containing include/ and lib/).

cmake_minimum_required(VERSION 3.22)
project(bp_spike CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ONNXRUNTIME_ROOT)
    message(FATAL_ERROR "Set -DONNXRUNTIME_ROOT=/path/to/onnxruntime-<os>-<arch>-<ver>")
endif()

add_executable(spike main.cpp)
target_include_directories(spike PRIVATE "${ONNXRUNTIME_ROOT}/include")
target_link_directories(spike PRIVATE "${ONNXRUNTIME_ROOT}/lib")
target_link_libraries(spike PRIVATE onnxruntime)

# Load libonnxruntime.so from next to the binary or from ONNXRUNTIME_ROOT/lib.
set_target_properties(spike PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib"
    INSTALL_RPATH "$ORIGIN")
