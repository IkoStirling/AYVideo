#pragma once
// VideoComponent.h — ECS component placeholder (V0.5).
//
// design.md §15 (foresight until V2+): the VideoSubsystem / component
// registration is a cross-module PR (AYGameLoop ISubSystem pattern,
// mirroring AudioSubSystem). V0.5 ships the POD type shape only so the
// ecs/ directory layout exists; nothing here is consumed by the engine
// yet.

#include <AYVideoMediaInfo.h>
#include <AYVideoTypes.h>
#include <aytime/Duration.h>

#include <string>

namespace ayt::video
{

// Per-entity video playback reference (V2+ cross-module integration).
// POD value type — the subsystem wires this into AYVideoPlayer.
struct VideoComponent
{
    // Player handle owned by the subsystem (V2+); V0.5 keeps the slot.
    std::string mediaPath;
    MediaInfo info{};
    ayt::time::Duration currentPosition{};
    bool autoPlay = false;
    bool loop = false;
    float volume = 1.0f;
};

} // namespace ayt::video
