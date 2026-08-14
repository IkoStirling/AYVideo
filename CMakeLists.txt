cmake_minimum_required(VERSION 3.12)

project(AYVideo
    VERSION 3.0.0
    DESCRIPTION "AYVideo — video playback submodule (V3 frame texture + color convert)"
)

# =============================================================================
# Module-level note: AYVideo V1 (2026-08-14) ships FFmpeg demux/decode +
# DecodeLoop + FrameQueue + pullFrame presentation. Without the FFmpeg
# package the module degrades to Null/Mock (AYVIDEO_HAS_FFMPEG unset).
# =============================================================================

set(MODULE_NAME AYVideo)

# Stub implementation files. Header-only types (result codes, POD
# carriers, sync clock contract) live in include/*.h; non-trivial bodies
# live here.
set(SRC_FILES
    src/AYVideoTypes.cpp             # toString() for VideoResult/VideoPixelFormat
    src/AYVideoBackendFactory.cpp    # make*Backend() construction
    src/AYVideoPlayer.cpp            # player state machine + V1 pipeline
    src/AYVideoSyncClock.cpp         # sync clock (engine + audio-master)
    src/DecodeLoop.cpp               # dedicated decode thread (std::thread)
    src/AYVideoSubSystem.cpp         # V2+ GameLoop subsystem (§15)
    src/VideoColorConvert.cpp        # V3 I420/NV12 → RGBA8 (§12)
)

# Backends: Null + Mock (V0.5) + FFmpeg (V1, design.md §7/§8).
# FFmpeg backends are excluded when the FFmpeg package is unavailable
# (e.g. machines without vcpkg) — the module degrades to Null/Mock.
set(BACKEND_FILES
    backend/NullDemuxer.cpp
    backend/MockDemuxer.cpp
    backend/NullDecoder.cpp
    backend/MockDecoder.cpp
    backend/CpuVideoFrameTexture.cpp
    backend/MockVideoFrameTexture.cpp
)

# V3 GPU frame texture — PRIVATE AYRenderer (design.md §12). Gated on
# the AYRenderer target existing (always present in the full tree;
# keeps standalone AYVideo builds from requiring Renderer).
if(TARGET AYRenderer)
    list(APPEND BACKEND_FILES backend/RendererVideoFrameTexture.cpp)
endif()

# vcpkg FindFFMPEG exposes FFMPEG_* variables (LIBRARIES / INCLUDE_DIRS /
# LIBRARY_DIRS) — it does NOT create FFmpeg::av* imported targets. Gate
# on FFMPEG_FOUND + wrap the variables in a local INTERFACE target so
# the rest of the module can link a single name (design.md §4.1 PRIVATE).
find_package(FFMPEG QUIET)
if(FFMPEG_FOUND)
    list(APPEND BACKEND_FILES
        backend/FFmpegDemuxer.cpp
        backend/FFmpegDecoder.cpp
    )
    if(NOT TARGET AYVideo_FFmpeg)
        add_library(AYVideo_FFmpeg INTERFACE)
        target_include_directories(AYVideo_FFmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(AYVideo_FFmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(AYVideo_FFmpeg INTERFACE ${FFMPEG_LIBRARIES})
    endif()
    set(AYVIDEO_FFMPEG_LIBS AYVideo_FFmpeg)
    set(AYVIDEO_HAS_FFMPEG ON)
    message(STATUS "AYVideo — FFmpeg backends ENABLED (find_package(FFMPEG) found)")
else()
    set(AYVIDEO_HAS_FFMPEG OFF)
    message(STATUS "AYVideo — FFmpeg backends DISABLED (FFmpeg package not found; Null/Mock only)")
endif()

# ECS component stubs (design.md §15; V0.5 ships type-only POD).
# No .cpp needed — pure POD, header-only.

add_library(${MODULE_NAME} STATIC ${SRC_FILES} ${BACKEND_FILES})

target_include_directories(${MODULE_NAME} PUBLIC
    # Public headers live flat under `include/`; backend seams live
    # under `interface/` (IAY prefix). Consumers write
    # `#include "AYVideoTypes.h"` or the umbrella `#include <AYVideo.h>`
    # which itself includes from include/ + interface/. Flat layout
    # mirrors AYAudio / AYVoxel (Phase 1+) conventions.
    #
    # PUBLIC is required because the target is STATIC — its own .cpp
    # files must be able to `#include "AYVideoXxx.h"` and find them in
    # PUBLIC include dirs (mirrors AY2D/CMakeLists.txt:64-71 rationale).
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
    $<INSTALL_INTERFACE:interface>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ecs>
    $<INSTALL_INTERFACE:ecs>
)

# V2+ dependency surface:
#   * AYTime     — Duration / TimePoint
#   * AYAudio    — PCM bridge (§11)
#   * AYGameLoop — ISubSystem for VideoSubSystem (§15)
target_link_libraries(${MODULE_NAME} PUBLIC AYTime AYAudio AYGameLoop)

# FFmpeg is a PRIVATE link (design.md §4.1): consumers of AYVideo never
# need libav* on their link line, and the public headers stay clean.
if(AYVIDEO_FFMPEG_LIBS)
    target_link_libraries(${MODULE_NAME} PRIVATE ${AYVIDEO_FFMPEG_LIBS})
    # Gates the makeFFmpeg* factory implementations in
    # src/AYVideoBackendFactory.cpp (they construct the PIMPL backends,
    # whose ctors live in the FFmpeg-compiled TUs).
    target_compile_definitions(${MODULE_NAME} PRIVATE AYVIDEO_HAS_FFMPEG=1)
endif()

# V3: GPU IVideoFrameTexture PRIVATE-links AYRenderer so public headers
# stay clean (G-01). Consumers that call makeRendererVideoFrameTexture
# must also link AYRenderer on the final executable (static-lib PRIVATE
# does not propagate).
if(TARGET AYRenderer)
    target_link_libraries(${MODULE_NAME} PRIVATE AYRenderer)
    target_compile_definitions(${MODULE_NAME} PRIVATE AYVIDEO_HAS_AYRENDERER=1)
    set(AYVIDEO_HAS_AYRENDERER ON)
    message(STATUS "AYVideo — Renderer GPU frame texture ENABLED (PRIVATE AYRenderer)")
else()
    set(AYVIDEO_HAS_AYRENDERER OFF)
    message(STATUS "AYVideo — Renderer GPU frame texture DISABLED (AYRenderer target missing)")
endif()

# ffmpeg-leak guard is wired on the static library so every
# `cmake --build .` inside AYVideo runs it (mirrors AYVoxel pattern).
add_dependencies(${MODULE_NAME} ayvideo_check_no_ffmpeg_in_public_headers)

# ----------------------------------------------------------------------------
# ffmpeg-leak guard target (design.md §4.3; AYVoxel
# CheckNoBgfxInPublicHeaders.cmake ancestor).
#
# Walks include/*.h + interface/*.h for any of the ffmpeg-family
# forbidden substrings (<libavformat/, <libavcodec/, ...).
# Exit 0 = no leak; exit 1 = at least one include directive in a public
# header matches a forbidden pattern. CI invokes this target by name.
#
# Phase 0 headerless state: include/ does not yet exist, so the guard
# trivially passes. V0.5 populates include/ + interface/ with public
# headers and the guard becomes load-bearing.
# ----------------------------------------------------------------------------
set(AYVIDEO_PUBLIC_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(AYVIDEO_INTERFACE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/interface")

add_custom_target(ayvideo_check_no_ffmpeg_in_public_headers
    COMMAND ${CMAKE_COMMAND}
        -DAYTGT_PUBLIC_HEADER_DIR="${AYVIDEO_PUBLIC_HEADER_DIR}"
        -DAYTGT_MODULE_NAME="${MODULE_NAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoFFmpegInPublicHeaders.cmake"
    COMMAND ${CMAKE_COMMAND}
        -DAYTGT_PUBLIC_HEADER_DIR="${AYVIDEO_INTERFACE_HEADER_DIR}"
        -DAYTGT_MODULE_NAME="${MODULE_NAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoFFmpegInPublicHeaders.cmake"
    COMMENT "AYVideo public-header ffmpeg-family leak guard (design.md §4.3)"
    VERBATIM
)

# ----------------------------------------------------------------------------
# AYTest integration (V0.5; mirror AYAudio / AYVoxel patterns).
#
#   * `enable_testing()` so CTest can find our test executables.
#   * `add_subdirectory(unittest)` gated on the directory existing, so a
#     sub-tree without Test_*.cpp is silently skipped.
# ----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unittest/CMakeLists.txt")
    enable_testing()
    add_subdirectory(unittest)
else()
    message(STATUS
        "AYVideo — unittest/ not present yet; "
        "skipping enable_testing/add_subdirectory for V0.5.")
endif()

# V3 engine demo (design.md §12) — Win32 + GameLoop + RendererSubSystem
# textured quad driven by RendererVideoFrameTexture.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/demo/CMakeLists.txt")
    if(TARGET AYRenderer)
        add_subdirectory(demo)
    else()
        message(STATUS "AYVideo — demo skipped (AYRenderer target missing)")
    endif()
endif()

message(STATUS "AYVideo v3.0.0 (V3 GPU frame texture + engine demo) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
