#pragma once
// AYVideo.h — umbrella include for the AYVideo video playback module.
//
// Consumers `#include <AYResource/assetsImpl/Video.h>` and get the full public surface.
// Mirrors AYVoxel.h / AYPhysics.h umbrella conventions.
//
// All public headers are FFmpeg-free (design.md §4.3 guard enforced).

#include "AYVideo/VideoAudioFrame.h" // §11 PCM carrier
#include "AYVideo/VideoFrame.h"      // §6.2 VideoPacket/VideoFrame carriers
#include "AYVideo/VideoMediaInfo.h"  // §6.1 MediaInfo snapshot
#include "AYVideo/VideoPlayer.h"     // §10 player state machine + control surface
#include "AYVideo/VideoSubSystem.h"  // §15 GameLoop subsystem
#include "AYVideo/VideoSyncClock.h"  // §9 A/V sync clock contract
#include "AYVideo/VideoTypes.h"      // §5 VideoResult + VideoPixelFormat
#include "AYVideo/IVideoBackendFactory.h" // backend + texture factories
#include "AYVideo/IVideoDecoder.h" // §8 decode seam
#include "AYVideo/IVideoDemuxer.h" // §7 demux seam
#include "AYVideo/IVideoFrameSink.h" // §12 present sink
#include "AYVideo/IVideoFrameTexture.h" // §12 frame texture seam
