#pragma once
// AYVideoSubtitle.h — soft-subtitle track metadata (V4 foresight N-08).
//
// Discovery-only: MediaInfo / player can enumerate tracks. Cue demux,
// ASS/libass render, and burn-in are OUT of this slice.

#include <cstdint>
#include <string>

namespace ayt::video
{

enum class SubtitleKind : uint8_t
{
    Unknown = 0,
    Text,     // SRT / WebVTT / plain text
    Ass,      // ASS / SSA
    Bitmap,   // DVD / PGS style
    Count
};

const char* toString(SubtitleKind kind) noexcept;

struct SubtitleTrackInfo
{
    int32_t streamIndex = -1;
    SubtitleKind kind = SubtitleKind::Unknown;
    std::string codec;     // container codec name ("subrip", "ass", ...)
    std::string language;  // ISO-ish tag when present ("eng")
    std::string title;     // optional stream title
};

} // namespace ayt::video
