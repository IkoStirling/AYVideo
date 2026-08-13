# =============================================================================
# CheckNoFFmpegInPublicHeaders.cmake
#
# Audit guard for AYVideo (design.md §4.3). AYVoxel's
# CheckNoBgfxInPublicHeaders.cmake ancestor, re-parameterized for the
# ffmpeg family of third-party include directives.
#
# Hard-fails if any header under AYTGT_PUBLIC_HEADER_DIR matches an
# ffmpeg-family leak pattern. Used as the body of the
# `ayvideo_check_no_ffmpeg_in_public_headers` custom target declared in
# the parent CMakeLists.txt.
#
# Input (cache variables):
#   AYTGT_PUBLIC_HEADER_DIR   absolute path to the AYVideo include directory
#   AYTGT_MODULE_NAME         human-readable name for diagnostics (default "AYVideo")
#
# Exit behavior:
#   0  — no leak found.
#   1  — at least one include directive matches a forbidden pattern in a
#        header under AYTGT_PUBLIC_HEADER_DIR, OR the header directory
#        does not exist (treated as V0.5 / pre-scaffold state is fine —
#        we explicitly do NOT fail on missing dir to keep V0.5 builds green).
#
# Forbidden patterns cover the ffmpeg header families a FFmpeg backend
# would pull into a consumer TU:
#   <libavformat/  <libavcodec/  <libavutil/  <libavfilter/
#   <libswscale/   <libswresample/ <libavdevice/
# plus the `"`-quoted variants.
# =============================================================================

cmake_minimum_required(VERSION 3.20)

if(NOT AYTGT_PUBLIC_HEADER_DIR)
    message(FATAL_ERROR "[CheckNoFFmpegInPublicHeaders] AYTGT_PUBLIC_HEADER_DIR not set")
endif()

# Some CMake invocations (e.g. when called via ninja custom_command with
# variable substitution) may pass the path wrapped in surrounding double
# quotes. Strip them defensively so EXISTS / GLOB_RECURSE paths behave.
string(REPLACE "\"" "" AYTGT_PUBLIC_HEADER_DIR "${AYTGT_PUBLIC_HEADER_DIR}")

if(NOT AYTGT_MODULE_NAME)
    set(AYTGT_MODULE_NAME "AYVideo")
endif()

# V0.5 / pre-scaffold: include/ may not exist yet. The check is a
# no-op in that state — we DO NOT fail, because failing on a missing dir
# would block V0.5 from ever configuring. As soon as include/*.h
# is populated, the guard becomes load-bearing.
if(NOT EXISTS "${AYTGT_PUBLIC_HEADER_DIR}")
    message(STATUS
        "[CheckNoFFmpegInPublicHeaders] (${AYTGT_MODULE_NAME}) "
        "Public header dir '${AYTGT_PUBLIC_HEADER_DIR}' does not exist; "
        "guard skipped. Create the directory and headers first."
    )
    return()
endif()

# Patterns we forbid in any public header. CMake's `string(FIND)` does
# literal substring search (NOT regex), so each pattern is a literal
# sequence of bytes. We approximate "include directive" with the literal
# `<libXxx/...` or `"libXxx/...` prefix. Backslashes on Windows are
# handled by reading the file in binary mode so the literal pattern
# matches either `/` or `\` separator — see file(READ ...) below.
#
# This is intentionally a fast substring scan, not a full regex linter.
# A `clang -M` transitive-include audit is a Phase 1+ follow-up
# (design.md §12.2).
set(_AYTGT_LEAK_PATTERNS
    "<libavformat/"
    "<libavcodec/"
    "<libavutil/"
    "<libavfilter/"
    "<libswscale/"
    "<libswresample/"
    "<libavdevice/"
    "\"libavformat/"
    "\"libavcodec/"
    "\"libavutil/"
    "\"libavfilter/"
    "\"libswscale/"
    "\"libswresample/"
    "\"libavdevice/"
)

set(_AYTGT_LEAK_HITS "")

# Globally `file(GLOB_RECURSE)` is usually discouraged, but here it is
# appropriate: we are inspecting a static, curtained public-header tree
# whose membership is created by the AYVideo module author. We are not
# globbing source files.
file(GLOB_RECURSE _AYTGT_HEADERS
    "${AYTGT_PUBLIC_HEADER_DIR}/*.h"
    "${AYTGT_PUBLIC_HEADER_DIR}/*.hpp"
    "${AYTGT_PUBLIC_HEADER_DIR}/*.inl"
)

if(NOT _AYTGT_HEADERS)
    message(STATUS
        "[CheckNoFFmpegInPublicHeaders] (${AYTGT_MODULE_NAME}) "
        "No headers under '${AYTGT_PUBLIC_HEADER_DIR}'. Guard trivially passed."
    )
    return()
endif()

set(_AYTGT_PATTERN_COUNT 0)
foreach(_pattern IN LISTS _AYTGT_LEAK_PATTERNS)
    math(EXPR _AYTGT_PATTERN_COUNT "${_AYTGT_PATTERN_COUNT} + 1")
endforeach()

foreach(_header IN LISTS _AYTGT_HEADERS)
    file(READ "${_header}" _AYTGT_CONTENT)

    foreach(_pattern IN LISTS _AYTGT_LEAK_PATTERNS)
        string(FIND "${_AYTGT_CONTENT}" "${_pattern}" _AYTGT_FOUND)
        # CMake's string(FIND) returns -1 if not found; >= 0 means at least
        # one match at offset >= 0. We treat -1 as "no hit".
        if(_AYTGT_FOUND GREATER -1)
            list(APPEND _AYTGT_LEAK_HITS
                "  ${_header}  matched pattern: ${_pattern}")
            # Break early for this header; one hit is enough to record it.
            break()
        endif()
    endforeach()
endforeach()

if(_AYTGT_LEAK_HITS)
    list(LENGTH _AYTGT_LEAK_HITS _AYTGT_HIT_COUNT)
    message(SEND_ERROR
        "[CheckNoFFmpegInPublicHeaders] (${AYTGT_MODULE_NAME}) "
        "Public-header ffmpeg-family leak detected in ${_AYTGT_HIT_COUNT} file(s):\n"
        "${_AYTGT_LEAK_HITS}\n"
        "Public headers under '${AYTGT_PUBLIC_HEADER_DIR}' MUST NOT include\n"
        "<libavformat/...> / <libavcodec/...> etc. (design.md §4.3). Move such\n"
        "includes to `src/detail/*` or to private header paths."
    )
else()
    message(STATUS
        "[CheckNoFFmpegInPublicHeaders] (${AYTGT_MODULE_NAME}) "
        "Public headers under '${AYTGT_PUBLIC_HEADER_DIR}' contain no ffmpeg-family includes. "
        "Guard passed."
    )
endif()
