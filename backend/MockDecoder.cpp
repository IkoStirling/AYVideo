#include "MockDecoder.h"

#include <aytime/Duration.h>

namespace ayt::video
{

namespace
{

constexpr int32_t kMockWidth = 320;
constexpr int32_t kMockHeight = 240;

} // namespace

MockDecoder::MockDecoder(int32_t frameCount)
    : _frameCount(frameCount < 0 ? 0 : frameCount)
    , _pixels(static_cast<size_t>(kMockWidth) * static_cast<size_t>(kMockHeight) * 4u, 0x7Fu)
{
}

VideoResult MockDecoder::open(const DecoderOpenParams& /*params*/)
{
    ++_openCount;
    _open = true;
    _emitted = 0;
    _flushed = false;
    _fedAny = false;
    _closed = false;
    return VideoResult::Ok;
}

void MockDecoder::close() noexcept
{
    _open = false;
    _closed = true;
}

bool MockDecoder::isOpen() const noexcept
{
    return _open;
}

VideoResult MockDecoder::feedPacket(const VideoPacket& /*packet*/)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    _fedAny = true;
    ++_feedCount;
    return VideoResult::Ok;
}

VideoResult MockDecoder::dequeueFrame(VideoFrame& outFrame)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    ++_dequeueCount;
    outFrame = VideoFrame{};

    if (!_fedAny)
    {
        // "No frame ready yet" contract state (design.md §8.3).
        return VideoResult::Ok;
    }
    if (_emitted >= _frameCount)
    {
        return _flushed ? VideoResult::EndOfStream : VideoResult::Ok;
    }

    outFrame.data = _pixels.data();
    outFrame.dataSize = static_cast<uint32_t>(_pixels.size());
    outFrame.width = kMockWidth;
    outFrame.height = kMockHeight;
    outFrame.stride = static_cast<uint32_t>(kMockWidth) * 4u;
    outFrame.format = VideoPixelFormat::RGBA8;
    outFrame.pts = ayt::time::Duration::fromUs(
        static_cast<std::int64_t>(_emitted) * 40'000); // 25 fps = 40 ms
    ++_emitted;
    return VideoResult::Ok;
}

VideoResult MockDecoder::flush()
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    ++_flushCount;
    _flushed = true;
    return VideoResult::Ok;
}

} // namespace ayt::video
