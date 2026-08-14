#include "MockVideoFrameTexture.h"

#include "../src/VideoColorConvert.h"

namespace ayt::video
{

VideoResult MockVideoFrameTexture::updateFromFrame(const VideoFrame& frame)
{
    _lastSource = frame.format;
    const VideoResult r = convertFrameToRgba8(frame, _rgba);
    if (r != VideoResult::Ok)
    {
        return r;
    }
    _width = static_cast<uint32_t>(frame.width);
    _height = static_cast<uint32_t>(frame.height);
    _format = VideoPixelFormat::RGBA8;
    _dirty = true;
    ++_updateCount;
    return VideoResult::Ok;
}

const uint8_t* MockVideoFrameTexture::rgba8Data() const noexcept
{
    return _rgba.empty() ? nullptr : _rgba.data();
}

uint32_t MockVideoFrameTexture::rgba8ByteSize() const noexcept
{
    return static_cast<uint32_t>(_rgba.size());
}

} // namespace ayt::video
