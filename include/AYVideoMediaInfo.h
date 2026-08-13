#pragma once
// AYVideoMediaInfo.h — container-level metadata snapshot.
//
// design.md §6.1: produced by IAYVideoDemuxer::getMediaInfo() after a
// successful open(). Plain value type — safe to copy across threads
// (player reads it, demux thread writes it once during open).

#include <string>

namespace ayt::video
{

struct MediaInfo
{
    // Video track (0/0 when the container has no video stream).
    int32_t width = 0;
    int32_t height = 0;
    double frameRate = 0.0;        // nominal video fps; 0 = unknown/variable

    // Container duration in seconds; 0 = unknown (live/undetermined).
    double durationSec = 0.0;

    bool hasVideo = false;
    bool hasAudio = false;

    // Codec names as reported by the container ("h264", "aac", ...);
    // empty when unknown. Diagnostic only — dispatch on capability, not
    // on this string (design.md §8.1).
    std::string videoCodec;
    std::string audioCodec;
};

} // namespace ayt::video
