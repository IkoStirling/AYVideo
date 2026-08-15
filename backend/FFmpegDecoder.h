#pragma once
// FFmpegDecoder.h — libavcodec video (+ optional audio) decoder.
//
// design.md §8 / §11: codec decode via avcodec. Single-threaded per
// instance (§4.4). When DecoderOpenParams::decodeAudio is true and the
// MediaInfo carries an audio track, an AAC (or named) audio codec is
// opened alongside video; PCM is resampled to interleaved F32 @ 48 kHz
// for the AYAudio bridge.
//
// The header is ffmpeg-free (PIMPL) — G-01 discipline.

#include <AYVideoAudioFrame.h>
#include <AYVideoFrame.h>
#include <AYVideoMediaInfo.h>
#include <AYVideoTypes.h>
#include <IAYVideoDecoder.h>

#include <memory>
#include <string>

namespace ayt::video
{

class FFmpegDecoder : public IAYVideoDecoder
{
public:
    FFmpegDecoder();
    ~FFmpegDecoder() override;

    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    VideoResult open(const DecoderOpenParams& params) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    VideoDecodeAccel activeDecodeAccel() const noexcept override;
    VideoResult feedPacket(const VideoPacket& packet) override;
    VideoResult dequeueFrame(VideoFrame& outFrame) override;
    VideoResult dequeueAudioFrame(AudioPcmFrame& outFrame) override;
    VideoResult flush() override;

    const char* lastErrorString() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::video
