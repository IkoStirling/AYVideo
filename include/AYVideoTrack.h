#pragma once
// AYVideoTrack.h — A/V track metadata (V4 N-10 multi-track selection).
//
// Discovery + deferred selection (applied on next play/seek). Seamless
// mid-play hot-swap without flush is OUT of this slice.

#include <cstdint>
#include <string>

namespace ayt::video
{

struct VideoTrackInfo
{
    int32_t streamIndex = -1;
    std::string codec;
    std::string language;
    std::string title;
    int32_t width = 0;
    int32_t height = 0;
    double frameRate = 0.0;
};

struct AudioTrackInfo
{
    int32_t streamIndex = -1;
    std::string codec;
    std::string language;
    std::string title;
    int32_t sampleRate = 0;
    int32_t channels = 0;
};

} // namespace ayt::video
