#include "MockDecoder.h"

#include <AYTime/Duration.h>

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

VideoResult MockDecoder::open(const DecoderOpenParams& params)
{
    ++_openCount;
    _params = params;
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

VideoResult MockDecoder::feedPacket(const VideoPacket& packet)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    if (!_failFeedDone && _failFeedAt >= 0
        && static_cast<int32_t>(_feedCount) == _failFeedAt)
    {
        _failFeedDone = true;
        ++_feedCount;
        return VideoResult::DecodeError;
    }
    // A-12: a real feed after flush clears the drain state and restarts
    // the scripted frame sequence at the packet's pts (seek / loop).
    if (_flushed)
    {
        _flushed = false;
        const std::int64_t ptsUs = packet.pts.toUs();
        std::int64_t idx = (ptsUs > 0) ? (ptsUs / 40'000) : 0;
        if (idx < 0)
        {
            idx = 0;
        }
        if (idx > _frameCount)
        {
            idx = _frameCount;
        }
        _emitted = static_cast<int32_t>(idx);
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
        // Flushed + no subsequent feed = drain complete (§8.2 EOS).
        // Never fed = "no frame ready yet" (§6.2 Ok + null).
        return _flushed ? VideoResult::EndOfStream : VideoResult::Ok;
    }
    if (_emitted >= _frameCount)
    {
        // Script exhausted: Ok + null until flush() (§8.2).
        return VideoResult::Ok;
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
    // Drain complete: next dequeue (without a new feed) reports EOS.
    // _emitted reset happens on the next feedPacket (A-12 seek restart).
    _fedAny = false;
    _emitted = 0;
    return VideoResult::Ok;
}

} // namespace ayt::video
