include_guard(GLOBAL)
# Shared codec/device dependency, extracted from NeoLIP without changing its pin.
if(TARGET neoshared::audio)
    return()
endif()
# miniaudio is built into the application; it is not downloaded by end users.
# An offline source checkout may be supplied at configure time.
set(NEOSHARED_MINIAUDIO_ROOT "" CACHE PATH "Offline miniaudio 0.11.23 source directory")
if(NOT NEOSHARED_MINIAUDIO_ROOT)
    include(FetchContent)
    FetchContent_Declare(neoshared_miniaudio_source
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG f40cf03f80cdb7e741d43e53b7e706e8c1394bcf
        GIT_PROGRESS FALSE
        SOURCE_SUBDIR neoshared-do-not-build-upstream-examples)
    FetchContent_MakeAvailable(neoshared_miniaudio_source)
    set(NEOSHARED_MINIAUDIO_ROOT "${neoshared_miniaudio_source_SOURCE_DIR}")
endif()
if(NOT EXISTS "${NEOSHARED_MINIAUDIO_ROOT}/miniaudio.h")
    message(FATAL_ERROR "miniaudio source is missing; the product cannot be built without its built-in audio decoder.")
endif()
file(READ "${NEOSHARED_MINIAUDIO_ROOT}/miniaudio.h" _neoshared_audio_version LIMIT 2048)
if(NOT _neoshared_audio_version MATCHES "v0\\.11\\.23")
    message(FATAL_ERROR "NeoShared audio requires miniaudio 0.11.23; use the pinned source version.")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/neoshared_miniaudio.cpp" "#define MINIAUDIO_IMPLEMENTATION\n#include \"miniaudio.h\"\n")
add_library(neoshared_audio_codec STATIC "${CMAKE_CURRENT_BINARY_DIR}/neoshared_miniaudio.cpp")
target_include_directories(neoshared_audio_codec SYSTEM PUBLIC "${NEOSHARED_MINIAUDIO_ROOT}")
target_compile_definitions(neoshared_audio_codec PUBLIC
    MA_NO_RESOURCE_MANAGER MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_GENERATION MA_NO_ENCODING MA_NO_FLAC)
if(EMSCRIPTEN)
    # Use miniaudio's native Emscripten/WebAudio backend and its own threading
    # stubs. MA_NO_THREADING disables device APIs; never combine it with ma_device.
    # No AudioWorklets, WASM workers, SharedArrayBuffer, or application pthreads.
    target_compile_options(neoshared_audio_codec PRIVATE -Wno-deprecated-declarations)
elseif(WIN32)
    target_link_libraries(neoshared_audio_codec PUBLIC ole32 uuid winmm)
elseif(APPLE)
    target_link_libraries(neoshared_audio_codec PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(neoshared_audio_codec PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
# Third-party warnings must not make our -Werror project checks compiler-specific.
if(MSVC)
    target_compile_options(neoshared_audio_codec PRIVATE /w)
else()
    target_compile_options(neoshared_audio_codec PRIVATE -w)
endif()

neo_apply_minimal_release(neoshared_audio_codec)

set(_neoshared_audio_source "${CMAKE_CURRENT_LIST_DIR}/../audio")
add_library(neoshared_audio STATIC
    "${_neoshared_audio_source}/Audio.cpp"
    "${_neoshared_audio_source}/Codecs.cpp"
    "${_neoshared_audio_source}/PlaybackCore.cpp"
    "${_neoshared_audio_source}/OpenALPlayback.cpp"
    "${_neoshared_audio_source}/PlaybackDevice.cpp")
add_library(neoshared::audio ALIAS neoshared_audio)
target_link_libraries(neoshared_audio PUBLIC neoshared::path neoshared_audio_codec)
target_compile_features(neoshared_audio PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neoshared_audio PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(neoshared_audio PRIVATE -Wall -Wextra -Wpedantic)
endif()
neo_apply_minimal_release(neoshared_audio)
