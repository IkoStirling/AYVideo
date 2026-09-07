#pragma once

#include <AYGameLoop/SubSystemModule.h>
#include <AYVideo/VideoSubSystem.h>

#include <string_view>

namespace ayt::video
{

inline constexpr std::string_view kVideoRuntimeModuleId =
    "AYVideo.Runtime";

// Optional presentation module. Audio is required because VideoSubSystem's
// GameLoop descriptor has a strict Audio dependency and may bridge decoded PCM
// into the registered AudioSubSystem.
class VideoRuntimeModule final : public ayt::game::SubSystemModule
{
public:
    explicit VideoRuntimeModule(VideoSubSystemOptions options = {});
};

} // namespace ayt::video
