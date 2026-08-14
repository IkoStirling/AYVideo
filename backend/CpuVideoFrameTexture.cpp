#include "CpuVideoFrameTexture.h"

#include "../src/VideoColorConvert.h"

namespace ayt::video
{

VideoResult CpuVideoFrameTexture::updateFromFrame(const VideoFrame& frame)
{
    const VideoResult r = convertFrameToRgba8(frame, _rgba);
    if (r != VideoResult::Ok)
    {
        return r;
    }
    _width = static_cast<uint32_t>(frame.width);
    _height = static_cast<uint32_t>(frame.height);
    _format = VideoPixelFormat::RGBA8;
    _dirty = true;
    return VideoResult::Ok;
}

const uint8_t* CpuVideoFrameTexture::rgba8Data() const noexcept
{
    return _rgba.empty() ? nullptr : _rgba.data();
}

uint32_t CpuVideoFrameTexture::rgba8ByteSize() const noexcept
{
    return static_cast<uint32_t>(_rgba.size());
}

} // namespace ayt::video
