#include "NullDecoder.h"

namespace ayt::video
{

VideoResult NullDecoder::open(const DecoderOpenParams& /*params*/)
{
    _open = true;
    _flushed = false;
    return VideoResult::Ok;
}

void NullDecoder::close() noexcept
{
    _open = false;
}

bool NullDecoder::isOpen() const noexcept
{
    return _open;
}

VideoResult NullDecoder::feedPacket(const VideoPacket& /*packet*/)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    return VideoResult::Ok;
}

VideoResult NullDecoder::dequeueFrame(VideoFrame& outFrame)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    outFrame = VideoFrame{};
    if (_flushed)
    {
        return VideoResult::EndOfStream;
    }
    // "No frame ready yet" contract state (design.md §8.3).
    return VideoResult::Ok;
}

VideoResult NullDecoder::flush()
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    _flushed = true;
    return VideoResult::Ok;
}

} // namespace ayt::video
