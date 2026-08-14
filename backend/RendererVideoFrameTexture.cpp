#include "RendererVideoFrameTexture.h"

#include "../src/VideoColorConvert.h"

namespace ayt::video
{

RendererVideoFrameTexture::RendererVideoFrameTexture(ayt::render::Renderer& renderer)
    : _renderer(renderer)
{
}

RendererVideoFrameTexture::~RendererVideoFrameTexture()
{
    destroyGpu();
}

void RendererVideoFrameTexture::destroyGpu() noexcept
{
    if (_handle.isValid())
    {
        _renderer.destroyTexture(_handle);
        _handle = {};
    }
}

VideoResult RendererVideoFrameTexture::updateFromFrame(const VideoFrame& frame)
{
    const VideoResult r = convertFrameToRgba8(frame, _rgba);
    if (r != VideoResult::Ok)
    {
        return r;
    }

    const uint32_t w = static_cast<uint32_t>(frame.width);
    const uint32_t h = static_cast<uint32_t>(frame.height);

    if (!_handle.isValid() || _width != w || _height != h)
    {
        destroyGpu();
        _handle = _renderer.createDynamicTextureRgba8(w, h);
        if (!_handle.isValid())
        {
            _width = 0;
            _height = 0;
            _format = VideoPixelFormat::Unknown;
            _rgba.clear();
            _dirty = false;
            return VideoResult::OutOfMemory;
        }
    }

    if (!_renderer.updateTextureFromRgba8(_handle, _rgba.data()))
    {
        return VideoResult::InvalidHandle;
    }

    _width = w;
    _height = h;
    _format = VideoPixelFormat::RGBA8;
    _dirty = true;
    return VideoResult::Ok;
}

const uint8_t* RendererVideoFrameTexture::rgba8Data() const noexcept
{
    return _rgba.empty() ? nullptr : _rgba.data();
}

uint32_t RendererVideoFrameTexture::rgba8ByteSize() const noexcept
{
    return static_cast<uint32_t>(_rgba.size());
}

uint64_t RendererVideoFrameTexture::gpuTextureId() const noexcept
{
    return _handle.id;
}

} // namespace ayt::video
