#pragma once
// AYVideoSubtitle.h — soft-subtitle track metadata + text cues.
//
// Track discovery ships in V4. Soft text cues (start/end/text) are the
// minimal present path for Mock / sidecar; ASS/libass burn-in and bitmap
// decode remain out of scope.

#include <cstdint>
#include <string>

#include <AYTime/Duration.h>

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

// Active soft cue window. Player filters by presentation clock.
struct SubtitleCue
{
    ayt::time::Duration start{};
    ayt::time::Duration end{};
    std::string text;
};

} // namespace ayt::video
