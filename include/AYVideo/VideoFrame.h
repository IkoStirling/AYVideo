#pragma once
// AYVideo/VideoFrame.h — data-carrier PODs for the decode pipeline.
//
// design.md §6.2: VideoPacket is the demux output (owned by the demuxer,
// valid until the next readNextPacket call); VideoFrame is the decoder
// output (owned by the decoder's frame pool, valid until the next
// dequeueFrame / flush call). Both are reference-semantics carriers —
// never copy the payload.

#include <AYVideo/VideoTypes.h>
#include <AYTime/Duration.h>

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

    bool isVideo = false;          // video elementary stream
    bool isSubtitle = false;       // text/bitmap subtitle packet (not audio)
    int64_t streamIndex = -1;      // container stream index (diagnostic)

    ayt::time::Duration pts{};     // presentation timestamp
    ayt::time::Duration dts{};     // decode timestamp (dts <= pts for B-frames)
    // Subtitle display duration when known (0 = decoder/default window).
    ayt::time::Duration duration{};
};

// ---------------------------------------------------------------------------
// A single decoded frame (raw pixel data).
// Lifetime contract: owned by IAYVideoDecoder; pointers are valid only
// until the next dequeueFrame() / flush() / close() on the same decoder.
// dequeueFrame() returning Ok with data == nullptr means "no frame ready
// yet" (codec still producing output); callers must not treat it as EOS.
//
// dataSize covers ALL planes (total bytes; V1 amendment A-05). For
// planar formats (I420/NV12) the per-plane byte offsets from `data` are
// in planeOffset[0..2] (planeOffset[0] == 0); `stride` is plane 0's row
// stride. Single-plane formats (RGBA8/BGRA8) leave the offsets 0.
// ---------------------------------------------------------------------------
struct VideoFrame
{
    const uint8_t* data = nullptr;
    uint32_t dataSize = 0;         // total bytes of all planes

    int32_t width = 0;
    int32_t height = 0;
    uint32_t stride = 0;           // bytes per row, plane 0
    uint32_t planeOffset[3] = {0, 0, 0};  // plane byte offsets from data
    VideoPixelFormat format = VideoPixelFormat::Unknown;

    ayt::time::Duration pts{};     // presentation timestamp (from packet)
};

} // namespace ayt::video
