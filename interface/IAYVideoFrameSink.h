#pragma once
// IAYVideoFrameSink.h — optional present callback (design.md §12).
//
// Fired on the player/subsystem thread after a frame is converted into
// an IVideoFrameTexture. Implementations must not re-enter the player
// control surface (same A-12 rule as state events).

#include <AYVideoTypes.h>
#include <IVideoFrameTexture.h>

#include <cstdint>

namespace ayt::video
{

class IAYVideoFrameSink
{
public:
    virtual ~IAYVideoFrameSink() = default;

    // `texture` is the staging texture just updated for this present.
    // `playbackId` is 0 when the caller is a bare AYVideoPlayer.
    virtual void onVideoFrame(uint32_t playbackId,
                              IVideoFrameTexture& texture) = 0;
};

} // namespace ayt::video
