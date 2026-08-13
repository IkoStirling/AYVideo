#pragma once
// AYVideo.h — umbrella include for the AYVideo video playback module.
//
// Consumers `#include <AYVideo.h>` and get the full V0.5 public
// surface. Mirrors AYVoxel.h / AYPhysics.h umbrella conventions.
//
// design.md §14.5 — V0.5 ships the type surface + Null/Mock backends
// only; the FFmpeg decode pipeline lands in V1+. All public headers
// are FFmpeg-free (design.md §4.3 guard enforced).

#include "include/AYVideoFrame.h"      // §6.2 VideoPacket/VideoFrame carriers
#include "include/AYVideoMediaInfo.h"  // §6.1 MediaInfo snapshot
#include "include/AYVideoPlayer.h"     // §10 player state machine + control surface
#include "include/AYVideoSyncClock.h"  // §9 A/V sync clock contract
#include "include/AYVideoTypes.h"      // §5 VideoResult + VideoPixelFormat
#include "interface/IAYVideoBackendFactory.h" // §17 backend construction
#include "interface/IAYVideoDecoder.h" // §8 decode seam
#include "interface/IAYVideoDemuxer.h" // §7 demux seam
