# ONNX Runtime acquisition for the ML note detector (Basic Pitch).
#
# Mirrors the NAM_AVAILABLE / RTNEURAL_AVAILABLE conditional pattern: if ONNX
# Runtime can be obtained, ONNXRUNTIME_AVAILABLE is set ON and the audio addon
# compiles with SLOPSMITH_ONNX_SUPPORT=1; otherwise the build still succeeds and
# the engine falls back to the YIN PitchDetector / ChordScorer (Constitution VII).
#
# Delivery: a pinned, prebuilt CPU release fetched at configure time with a
# pinned URL + SHA-256 (Constitution V — reproducible builds). For fully offline
# / pre-seeded builds, set -DSLOPSMITH_ONNXRUNTIME_ROOT=/path/to/extracted/onnxruntime
# (the directory containing include/ and lib/) and the fetch is skipped.
#
# Exports (cache/parent scope):
#   ONNXRUNTIME_AVAILABLE     ON/OFF
#   ONNXRUNTIME_INCLUDE_DIR   header directory
#   ONNXRUNTIME_IMPORT_LIB    library to link against
#   ONNXRUNTIME_RUNTIME_LIB   shared lib that must sit next to slopsmith_audio.node

set(ONNXRUNTIME_VERSION "1.20.1" CACHE STRING "Pinned ONNX Runtime version")

# --- Resolve the prebuilt asset for this OS/arch --------------------------
set(_ort_base "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}")
set(_ort_ok ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_ort_asset "onnxruntime-win-x64-${ONNXRUNTIME_VERSION}")
    set(_ort_ext "zip")
    set(_ort_sha "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581")
    set(_ort_import "onnxruntime.lib")
    set(_ort_runtime "onnxruntime.dll")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_ort_ext "tgz")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_ort_asset "onnxruntime-osx-arm64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "b678fc3c2354c771fea4fba420edeccfba205140088334df801e7fc40e83a57a")
    else()
        set(_ort_asset "onnxruntime-osx-x86_64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "0f73006813af2a1a5d1723ed7dfb694fc629d15037124081bb61b7bf7d99fc78")
    endif()
    set(_ort_import "libonnxruntime.dylib")
    set(_ort_runtime "libonnxruntime.${ONNXRUNTIME_VERSION}.dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ort_ext "tgz")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_ort_asset "onnxruntime-linux-aarch64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "ae4fedbdc8c18d688c01306b4b50c63de3445cdf2dbd720e01a2fa3810b8106a")
    else()
        set(_ort_asset "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105")
    endif()
    set(_ort_import "libonnxruntime.so")
    # SONAME is libonnxruntime.so.1 — that exact name must sit next to the addon.
    set(_ort_runtime "libonnxruntime.so.1")
else()
    set(_ort_ok OFF)
    message(STATUS "ONNX Runtime: unsupported platform '${CMAKE_SYSTEM_NAME}' — ML note detection disabled")
endif()

# --- Obtain the runtime: explicit root override, or pinned fetch ----------
set(_ort_root "")

if(_ort_ok AND DEFINED SLOPSMITH_ONNXRUNTIME_ROOT)
    if(EXISTS "${SLOPSMITH_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        set(_ort_root "${SLOPSMITH_ONNXRUNTIME_ROOT}")
        message(STATUS "ONNX Runtime: using prepopulated root ${_ort_root}")
    else()
        message(WARNING "SLOPSMITH_ONNXRUNTIME_ROOT='${SLOPSMITH_ONNXRUNTIME_ROOT}' "
                        "has no include/onnxruntime_cxx_api.h — ignoring")
    endif()
endif()

# Download + extract the prebuilt archive. Every failure mode here is SOFT —
# offline machine, download error, hash mismatch, bad layout all just disable
# ML detection (YIN fallback). Configure must never abort on this, so we use
# file(DOWNLOAD) with a status check rather than FetchContent, which raises a
# FATAL_ERROR when the archive can't be fetched.
if(_ort_ok AND _ort_root STREQUAL "")
    set(_ort_url     "${_ort_base}/${_ort_asset}.${_ort_ext}")
    set(_ort_archive "${CMAKE_BINARY_DIR}/_deps/${_ort_asset}.${_ort_ext}")
    set(_ort_extract "${CMAKE_BINARY_DIR}/_deps/onnxruntime")
    set(_ort_header  "${_ort_extract}/${_ort_asset}/include/onnxruntime_cxx_api.h")

    # On a clean build tree _deps does not exist yet — create it so the
    # file(DOWNLOAD) below can open its destination file instead of failing
    # and silently disabling ONNX support on every fresh configure.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")

    # Download — cached: a prior configure's archive with a matching hash is
    # reused, so reconfigure / clean-tree rebuilds don't re-fetch.
    set(_ort_have OFF)
    if(EXISTS "${_ort_archive}")
        file(SHA256 "${_ort_archive}" _ort_got)
        if(_ort_got STREQUAL "${_ort_sha}")
            set(_ort_have ON)
        endif()
    endif()
    if(NOT _ort_have)
        message(STATUS "ONNX Runtime: downloading ${_ort_asset}.${_ort_ext}")
        file(DOWNLOAD "${_ort_url}" "${_ort_archive}" STATUS _ort_dlst TLS_VERIFY ON)
        list(GET _ort_dlst 0 _ort_dlcode)
        if(_ort_dlcode EQUAL 0 AND EXISTS "${_ort_archive}")
            file(SHA256 "${_ort_archive}" _ort_got)
            if(_ort_got STREQUAL "${_ort_sha}")
                set(_ort_have ON)
            else()
                message(WARNING "ONNX Runtime: SHA-256 mismatch on ${_ort_asset} "
                                "— ML note detection disabled (YIN fallback)")
            endif()
        else()
            list(GET _ort_dlst 1 _ort_dlmsg)
            message(STATUS "ONNX Runtime: download failed (${_ort_dlmsg}) "
                           "— ML note detection disabled (YIN fallback)")
        endif()
    endif()

    # Extract — only when the archive is good and not already unpacked.
    if(_ort_have AND NOT EXISTS "${_ort_header}")
        file(REMOVE_RECURSE "${_ort_extract}")
        file(MAKE_DIRECTORY "${_ort_extract}")
        file(ARCHIVE_EXTRACT INPUT "${_ort_archive}" DESTINATION "${_ort_extract}")
    endif()

    if(_ort_have AND EXISTS "${_ort_header}")
        set(_ort_root "${_ort_extract}/${_ort_asset}")
        message(STATUS "ONNX Runtime ${ONNXRUNTIME_VERSION}: ready (${_ort_asset})")
    elseif(_ort_have)
        message(WARNING "ONNX Runtime: unexpected archive layout "
                        "— ML note detection disabled (YIN fallback)")
    endif()
endif()

# --- Publish results -------------------------------------------------------
# Verify the link + runtime libraries actually exist before declaring ONNX
# available. A prepopulated SLOPSMITH_ONNXRUNTIME_ROOT (or an unexpected
# archive layout) could carry the headers but miss/rename the libs — without
# this check configure would pass and the build would only fail later at link
# or the post-build copy, defeating the intended soft-fall to YIN.
set(_ort_import_path  "${_ort_root}/lib/${_ort_import}")
set(_ort_runtime_path "${_ort_root}/lib/${_ort_runtime}")
if(_ort_ok AND NOT _ort_root STREQUAL ""
   AND NOT (EXISTS "${_ort_import_path}" AND EXISTS "${_ort_runtime_path}"))
    message(WARNING "ONNX Runtime: headers found but lib '${_ort_import}' / "
                    "'${_ort_runtime}' missing under ${_ort_root}/lib "
                    "— ML note detection disabled (YIN fallback)")
    set(_ort_root "")
endif()

if(_ort_ok AND NOT _ort_root STREQUAL "")
    set(ONNXRUNTIME_AVAILABLE   ON                       CACHE INTERNAL "")
    set(ONNXRUNTIME_INCLUDE_DIR "${_ort_root}/include"   CACHE INTERNAL "")
    set(ONNXRUNTIME_IMPORT_LIB  "${_ort_import_path}"    CACHE INTERNAL "")
    set(ONNXRUNTIME_RUNTIME_LIB "${_ort_runtime_path}"   CACHE INTERNAL "")
    message(STATUS "ONNX Runtime available — ML note detection enabled")
else()
    set(ONNXRUNTIME_AVAILABLE OFF CACHE INTERNAL "")
    message(STATUS "ONNX Runtime not available — ML note detection disabled (YIN fallback)")
endif()
