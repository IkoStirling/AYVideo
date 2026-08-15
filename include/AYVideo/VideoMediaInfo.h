#pragma once
// AYVideo/VideoMediaInfo.h — container-level metadata snapshot.
//
// design.md §6.1: produced by IAYVideoDemuxer::getMediaInfo() after a
// successful open(). Plain value type — safe to copy across threads
// (player reads it, demux thread writes it once during open).

#include <cstdint>
#include <string>
#include <vector>

#include <AYVideo/VideoSubtitle.h>
#include <AYVideo/VideoTrack.h>

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
    bool hasSubtitles = false;

    // Codec names as reported by the container ("h264", "aac", ...);
    // empty when unknown. Diagnostic only — dispatch on capability, not
    // on this string (design.md §8.1). Active-track summary (index 0
    // default; refreshed after setActive* + play/seek).
    std::string videoCodec;
    std::string audioCodec;

    // Soft-subtitle tracks (V4 discovery; empty when none).
    std::vector<SubtitleTrackInfo> subtitleTracks;

    // Soft text cues for the active/discovered text track (Mock fills
    // these; FFmpeg packet→cue decode is a later slice). Empty = none.
    std::vector<SubtitleCue> softSubtitleCues;

    // All video/audio streams (V4 N-10). Scalar fields above mirror the
    // currently active selection.
    std::vector<VideoTrackInfo> videoTracks;
    std::vector<AudioTrackInfo> audioTracks;

    // Codec extradata (AVCodecParameters.extradata) — required by many
    // codecs (mpeg4 VOL, h264 SPS/PPS in mp4) before the first feed.
    // Empty when the container carries in-band headers only.
    std::vector<uint8_t> videoExtradata;

    // Audio track metadata (V2 PCM bridge, design.md §11). 0 when absent.
    int32_t audioSampleRate = 0;
    int32_t audioChannels = 0;
    std::vector<uint8_t> audioExtradata;
};

} // namespace ayt::video
