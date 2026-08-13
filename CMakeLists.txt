cmake_minimum_required(VERSION 3.12)

project(AYVideo
    VERSION 0.5.0
    DESCRIPTION "AYVideo — video playback submodule (V0.5 stub-only skeleton)"
)

# =============================================================================
# Module-level guard: AYVideo is V0.5 docs+stubs as of v0.5.0 (2026-08-13).
# The target list below is the stub-only skeleton promised in design.md
# §17 V0.5 ship checklist + §14.5 directory layout.
#
# What is wired here in V0.5:
#   1. The library target (`AYVideo`) as STATIC with real stub .cpp
#      files — mirrors AYAudio / AYVoxel sibling convention.
#   2. The public-header ffmpeg-leak guard (design.md §4.3).
#   3. The backend set (Null + Mock for demux + decode) — every backend
#      always builds; the FFmpeg backend arrives in V1 (design.md §8.1).
#   4. The unittest subdirectory via the existing `EXISTS` gate pattern.
#
# What is NOT wired in V0.5 (per design.md §17 V0.5 entry):
#   - FFmpeg demux/decode backends (V1)
#   - frame queue / decode worker thread / pipeline wiring (V1)
#   - A/V sync audio-master implementation (V2)
#   - AYAudio PCM bridge / AYRenderer texture bridge (V2/V3 cross-module)
#   - VideoSubsystem ECS integration (V2+ cross-module)
# =============================================================================

set(MODULE_NAME AYVideo)

# Stub implementation files. Header-only types (result codes, POD
# carriers, sync clock contract) live in include/*.h; non-trivial bodies
# live here.
set(SRC_FILES
    src/AYVideoTypes.cpp             # toString() for VideoResult/VideoPixelFormat
    src/AYVideoBackendFactory.cpp    # make*Backend() construction
    src/AYVideoPlayer.cpp            # player state machine + control surface
    src/AYVideoSyncClock.cpp         # sync clock contract stub (engine-clock path)
)

# Backends (V0.5): Null + Mock for every role — demux + decode.
# FFmpeg backends land in V1 (design.md §8.1).
set(BACKEND_FILES
    backend/NullDemuxer.cpp
    backend/MockDemuxer.cpp
    backend/NullDecoder.cpp
    backend/MockDecoder.cpp
)

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
)

# V0.5 dependency surface is intentionally minimal:
#   * AYTime — Duration / TimePoint for pts + sync clock (design.md §2.5
#     normative: AYTime v1.2 value types are the module's time unit).
#   * No AYIO / AYTask / AYAudio / AYRenderer in V0.5 — the FFmpeg
#     backend and pipeline wiring (V1+) consume them.
# Future versions add:
#   * AYIO       (V1 demux byte-stream reads, design.md §7.2)
#   * AYTask     (V1 decode worker thread, design.md §8.2)
#   * AYAudio    (V2 PCM stream bridge, design.md §11)
#   * AYRenderer (V3 texture upload bridge, design.md §12 — via
#                 cross-module PR, NOT a direct AYRenderer link in V0.5)
target_link_libraries(${MODULE_NAME} PUBLIC AYTime)

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

message(STATUS "AYVideo v0.5.0 (V0.5 stub-only skeleton) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
