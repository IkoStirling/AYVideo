#pragma once
// IAYVideoBackendFactory.h — backend construction entry points.
//
// Mirrors AYAudioBackendFactory (sibling convention): free functions
// exposed individually so tests and tools can swap backends without
// going through a settings object. The FFmpeg backend lands in V1
// (design.md §8); Null/Mock backends ship in V0.5.
//
// NOTE: public headers must stay FFmpeg-free (design.md §4.3 — the
// `ayvideo_check_no_ffmpeg_in_public_headers` guard enforces this).
// Concrete backend types live in backend/ and are created only through
// these factory functions.

#include <IAYVideoDecoder.h>
#include <IAYVideoDemuxer.h>

#include <memory>

namespace ayt::video
{

// V0.5 backends — silent no-op (Null) / scripted sequences (Mock).
std::unique_ptr<IAYVideoDemuxer> makeNullDemuxer();
std::unique_ptr<IAYVideoDemuxer> makeMockDemuxer(int32_t packetCount);
std::unique_ptr<IAYVideoDecoder> makeNullDecoder();
std::unique_ptr<IAYVideoDecoder> makeMockDecoder(int32_t frameCount);

// V1: FFmpeg backends (design.md §7/§8). Constructed via libav*; the
// concrete types live in backend/ (ffmpeg-free headers, PIMPL) so the
// public surface stays clean (G-01).
std::unique_ptr<IAYVideoDemuxer> makeFFmpegDemuxer();
std::unique_ptr<IAYVideoDecoder> makeFFmpegDecoder();

} // namespace ayt::video
