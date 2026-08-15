#pragma once
// AYVideo/IVideoDecoder.h — codec decode seam.
//
// design.md §8: turns demuxed packets into raw frames. A decoder owns
// its frame pool — frame pointers returned by dequeueFrame are valid
// until the next call on the same decoder.
//
// Thread contract (design.md §14): a decoder instance is confined to a
// single thread (the decode thread). Codecs are not thread-safe;
// never drive one decoder from two threads.

#include <AYVideo/VideoAudioFrame.h>
#include <AYVideo/VideoFrame.h>
#include <AYVideo/VideoMediaInfo.h>
#include <AYVideo/VideoTypes.h>

#include <string>

namespace ayt::video
{

struct DecoderOpenParams
{
    std::string codecName;       // "h264", "aac", ... — dispatch on this
                                 // to a concrete codec; unknown names
                                 // return UnsupportedFormat
    MediaInfo media;             // container metadata hint (resolution,
                                 // fps, duration)
    bool decodeAudio = false;    // V2: false = video-only pipeline

    // V6: preferred hardware decode path (FFmpeg backends). None = soft.
    // Auto / explicit HW try first; on failure soft-open when
    // allowSoftwareFallback is true (CI / headless safe default).
    VideoDecodeAccel preferredAccel = VideoDecodeAccel::None;
    bool allowSoftwareFallback = true;
};

class IAYVideoDecoder
{
public:
    virtual ~IAYVideoDecoder() = default;

    // Opens the codec. Must be called once before any feedPacket call;
    // open() on an open decoder returns InvalidState (call close() first).
    virtual VideoResult open(const DecoderOpenParams& params) = 0;

    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;

    // Actual accel in use after open() (None when soft / unopened).
    virtual VideoDecodeAccel activeDecodeAccel() const noexcept
    {
        return VideoDecodeAccel::None;
    }

    // Feeds one demuxed packet (compressed data). Packets must arrive in
    // stream order. Returns Ok even when no frame is produced yet (codec
    // delay / B-frames) — frames are collected via dequeueFrame /
    // dequeueAudioFrame. Audio packets are ignored when decodeAudio was
    // false at open (V1 video-only).
    virtual VideoResult feedPacket(const VideoPacket& packet) = 0;

    // Dequeues the next decoded video frame if one is ready:
    //   Ok                 — outFrame filled
    //   Ok + data == nullptr — no frame ready yet (still decoding);
    //                         call again after feeding more packets
    //   EndOfStream        — decoder drained after flush()
    virtual VideoResult dequeueFrame(VideoFrame& outFrame) = 0;

    // Dequeues the next decoded PCM chunk (V2). Default: always Ok +
    // null (video-only backends). Same lifetime contract as video frames.
    virtual VideoResult dequeueAudioFrame(AudioPcmFrame& outFrame)
    {
        outFrame = AudioPcmFrame{};
        return VideoResult::Ok;
    }

    // Flushes codec-internal delay (e.g. trailing B-frames) and resets
    // the codec state for a new GOP / seek boundary. After flush, feed
    // packets for the new position. Returns EndOfStream from
    // dequeueFrame until the first frame of the new sequence decodes.
    virtual VideoResult flush() = 0;
};

} // namespace ayt::video
