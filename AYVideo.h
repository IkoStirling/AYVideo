#pragma once
// AYVideo.h — umbrella include for the AYVideo video playback module.
//
// Consumers `#include <AYVideo.h>` and get the full public surface.
// Mirrors AYVoxel.h / AYPhysics.h umbrella conventions.
//
// All public headers are FFmpeg-free (design.md §4.3 guard enforced).

#include "include/AYVideoAudioFrame.h" // §11 PCM carrier
#include "include/AYVideoFrame.h"      // §6.2 VideoPacket/VideoFrame carriers
#include "include/AYVideoMediaInfo.h"  // §6.1 MediaInfo snapshot
#include "include/AYVideoPlayer.h"     // §10 player state machine + control surface
#include "include/AYVideoSubSystem.h"  // §15 GameLoop subsystem
#include "include/AYVideoSyncClock.h"  // §9 A/V sync clock contract
#include "include/AYVideoTypes.h"      // §5 VideoResult + VideoPixelFormat
#include "interface/IAYVideoBackendFactory.h" // backend + texture factories
#include "interface/IAYVideoDecoder.h" // §8 decode seam
#include "interface/IAYVideoDemuxer.h" // §7 demux seam
#include "interface/IAYVideoFrameSink.h" // §12 present sink
#include "interface/IVideoFrameTexture.h" // §12 frame texture seam
