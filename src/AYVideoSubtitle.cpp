#include <AYVideoSubtitle.h>

namespace ayt::video
{

const char* toString(SubtitleKind kind) noexcept
{
    switch (kind)
    {
    case SubtitleKind::Unknown: return "Unknown";
    case SubtitleKind::Text:    return "Text";
    case SubtitleKind::Ass:     return "Ass";
    case SubtitleKind::Bitmap:  return "Bitmap";
    case SubtitleKind::Count:   return "Count";
    }
    return "Unknown";
}

} // namespace ayt::video
