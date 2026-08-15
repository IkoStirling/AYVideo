cmake_minimum_required(VERSION 3.15)

project(AYVideo_Tests
    VERSION 0.5.0
    DESCRIPTION "AYVideo unit tests (V0.5 stub-only skeleton; TU placeholders per design.md §17)"
)

set(TEST_TARGET AYVideo_Tests)

# ---------------------------------------------------------------------------
# V0.5 test target layout (mirrors AYVoxel/unittest/CMakeLists.txt +
# AYAudio sibling pattern):
#
#   * Each Test_<Subject>.cpp is a STAND-ALONE translation unit (one
#     file = one TU) — avoids MSVC include-order issues per AYAudio
#     convention. TU isolation also means each file gets fresh state.
#
#   * The AYVideo module is STATIC in V0.5 (real stub .cpp files). Tests
#     link against it as a normal static library; their PUBLIC include
#     dir comes through `target_link_libraries(... AYVideo)`.
#
#   * Backend headers (Null*/Mock*) are reached through the module root
#     include path (backend/*.h lives one level up from unittest/).
#
#   * AYTest comes from AYFoundation/AYTest/CMakeLists.txt.
# ---------------------------------------------------------------------------

add_executable(${TEST_TARGET}
    main.cpp
    # V0.5 stubs — exercise the public surface (design.md §17 + §19).
    Test_VideoTypes.cpp          # VideoResult 13 codes + toString + sentinel
    Test_MediaInfo.cpp           # MediaInfo POD defaults + round-trip
    Test_Frame.cpp               # VideoPacket/VideoFrame carriers + pts typing
    Test_NullBackends.cpp        # NullDemuxer/NullDecoder contract
    Test_MockBackends.cpp        # Mock sequence replay + EOS + counters
    Test_PlayerState.cpp         # state machine + control surface + fault injection
    Test_SyncClock.cpp           # sync clock contract (fake now provider)
    Test_VideoSubSystem.cpp      # V2+ GameLoop subsystem smoke
    Test_FrameTexture.cpp        # V3 color convert + IVideoFrameTexture
    Test_ErrorRecovery.cpp       # V4 soft-skip mid-stream Demux/Decode errors
    Test_SubtitleTracks.cpp      # V4 soft-subtitle track discovery
    Test_MediaTracks.cpp         # V4 A/V multi-track selection
    Test_MemoryPressure.cpp      # V4 FrameQueue DropOldest + size bounds
    Test_NetworkStream.cpp       # V5 HTTP buffering + reconnect (Mock)
    Test_ScrubRegression.cpp     # Scrub→play CatchUp + twitch + A/B open
    Test_FrameStep.cpp           # Editor stepFrames ±1 while Paused
    Test_DecodeAccel.cpp         # V6 preferredDecodeAccel (Mock)
)

# V3 GPU bridge — needs AYRenderer Noop backend (design.md §12).
if(AYVIDEO_HAS_AYRENDERER)
    target_sources(${TEST_TARGET} PRIVATE
        Test_RendererFrameTexture.cpp
    )
    target_link_libraries(${TEST_TARGET} PRIVATE AYRenderer)
    target_compile_definitions(${TEST_TARGET} PRIVATE AYVIDEO_HAS_AYRENDERER=1)
endif()

target_link_libraries(${TEST_TARGET} PRIVATE
    AYVideo
    AYTest
    AYTime
    AYAudio
)

# V1 FFmpeg backends (design.md §7/§8) — Q7a synthetic-byte stub:
# the test generates its own mp4 via libavformat muxing, so these TUs
# link libav* PRIVATEly via the module-local AYVideo_FFmpeg INTERFACE
# target (parent CMakeLists creates it when FFMPEG_FOUND).
if(AYVIDEO_HAS_FFMPEG)
    target_sources(${TEST_TARGET} PRIVATE
        FFmpegTestMedia.cpp      # shared synthetic mp4 generator (Q7a)
        Test_FFmpegDemuxer.cpp   # FFmpegDemuxer against generated mp4
        Test_FFmpegDecoder.cpp   # FFmpegDecoder feed/dequeue/flush/EOS
        Test_PlayerPlayback.cpp  # full pipeline + clock-gated presentation
        Test_DecodeThread.cpp    # §16.3 stress: 400 frames x 3 rounds
        Test_AudioBridge.cpp     # V2 attachAudioEngine smoke
    )
    target_link_libraries(${TEST_TARGET} PRIVATE AYVideo_FFmpeg)
endif()

target_include_directories(${TEST_TARGET} PRIVATE
    # Reach backend/*.h through include relative paths (Test_NullBackends.cpp
    # uses `#include "NullDemuxer.h"` from the AYVideo root). The
    # unittest/ directory's parent is the module root.
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_definitions(${TEST_TARGET} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(${TEST_TARGET} PRIVATE /W4 /wd4200)
else()
    target_compile_options(${TEST_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
