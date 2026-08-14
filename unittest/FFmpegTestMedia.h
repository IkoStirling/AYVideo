#pragma once
// FFmpegTestMedia.h — shared synthetic-media generator for V1 FFmpeg
// backend tests (Q7a synthetic-byte stub, design.md §19).
//
// Generates a minimal mp4 at runtime via the libavformat muxing API —
// native MPEG-4 Part 2 video (+ optional native AAC audio). Zero
// external files, CI-deterministic (both encoders are deterministic).
// h264 is unavailable in the vcpkg ffmpeg build (--disable-libx264/
// --disable-libopenh264); the validation matrix uses mpeg4+aac until an
// h264 sample exists (design.md §20 A-04).

#include <string>

namespace ayt::testmedia
{

constexpr int32_t kGenWidth = 64;
constexpr int32_t kGenHeight = 48;
constexpr int32_t kGenFrames = 12;
constexpr double kGenFps = 25.0;

struct GeneratedClip
{
    std::string path;
    int32_t width = 0;
    int32_t height = 0;
    int32_t videoFrames = 0;
    double fps = 0.0;
    bool hasAudio = false;
};

// Generates (or overwrites) `path` with a minimal mp4. Returns false
// and fills `outError` on failure.
bool generateClip(const std::string& path, bool withAudio,
                  int32_t frames, int32_t width, int32_t height, double fps,
                  std::string& outError);

// Temp path under the AYTest tmp dir.
std::string tempClipPath(const char* tag);

// Convenience: generate + CHECK_TRUE (test-helper; returns a valid
// clip on success, empty path on failure).
GeneratedClip makeClip(bool withAudio);

} // namespace ayt::testmedia
