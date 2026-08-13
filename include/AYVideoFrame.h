#pragma once
// AYVideoFrame.h — data-carrier PODs for the decode pipeline.
//
// design.md §6.2: VideoPacket is the demux output (owned by the demuxer,
// valid until the next readNextPacket call); VideoFrame is the decoder
// output (owned by the decoder's frame pool, valid until the next
// dequeueFrame / flush call). Both are reference-semantics carriers —
// never copy the payload.

#include <AYVideoTypes.h>
#include <aytime/Duration.h>

#include <cstdint>

namespace ayt::video
{

// ---------------------------------------------------------------------------
// A single demuxed packet (compressed elementary stream chunk).
// Lifetime contract: owned by IAYVideoDemuxer; pointers are valid only
// until the next readNextPacket() / close() / seek() on the same demuxer.
// ---------------------------------------------------------------------------
struct VideoPacket
{
    const uint8_t* data = nullptr;
    uint32_t size = 0;

    bool isVideo = false;          // false = audio track packet
    int64_t streamIndex = -1;      // container stream index (diagnostic)

    ayt::time::Duration pts{};     // presentation timestamp
    ayt::time::Duration dts{};     // decode timestamp (dts <= pts for B-frames)
};

// ---------------------------------------------------------------------------
// A single decoded frame (raw pixel data).
// Lifetime contract: owned by IAYVideoDecoder; pointers are valid only
// until the next dequeueFrame() / flush() / close() on the same decoder.
// dequeueFrame() returning Ok with data == nullptr means "no frame ready
// yet" (codec still producing output); callers must not treat it as EOS.
// ---------------------------------------------------------------------------
struct VideoFrame
{
    const uint8_t* data = nullptr;
    uint32_t dataSize = 0;

    int32_t width = 0;
    int32_t height = 0;
    uint32_t stride = 0;           // bytes per row, plane 0 (all planes
                                   // packed contiguously in data)
    VideoPixelFormat format = VideoPixelFormat::Unknown;

    ayt::time::Duration pts{};     // presentation timestamp (from packet)
};

} // namespace ayt::video
