#pragma once
// FFmpegDecoder.h — libavcodec video decoder (V1).
//
// design.md §8: codec decode via avcodec. Single-threaded per instance
// (§4.4 — ffmpeg decoders are not thread-safe). Output frames follow
// the §6.2 contract: `dequeueFrame` returns Ok + null data when no
// frame is ready, and EndOfStream after flush().
//
// Frame pixel data is owned by the decoder (internal frame pool,
// §4.5): valid until the next dequeueFrame/flush call.
//
// The header is ffmpeg-free (PIMPL) — G-01 discipline.

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
    VideoResult feedPacket(const VideoPacket& packet) override;
    VideoResult dequeueFrame(VideoFrame& outFrame) override;
    VideoResult flush() override;

    // Diagnostics (tests): last av* error string, or "".
    const char* lastErrorString() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::video
