#pragma once
// RendererVideoFrameTexture.h — GPU IVideoFrameTexture (design.md §12).
//
// PRIVATE AYRenderer bridge: converts frames to RGBA8 staging, then
// createDynamicTextureRgba8 + updateTextureFromRgba8. Public headers
// stay AYRenderer-free; demos/tests that need TextureHandle include
// this backend header.

#include <AYVideo/IVideoFrameTexture.h>

#include <AYRenderer.h>

#include <cstdint>
#include <vector>

namespace ayt::video
{

class RendererVideoFrameTexture final : public IVideoFrameTexture
{
public:
    explicit RendererVideoFrameTexture(ayt::render::Renderer& renderer);
    ~RendererVideoFrameTexture() override;

    RendererVideoFrameTexture(const RendererVideoFrameTexture&) = delete;
    RendererVideoFrameTexture& operator=(const RendererVideoFrameTexture&) = delete;

    uint32_t width() const noexcept override { return _width; }
    uint32_t height() const noexcept override { return _height; }
    VideoPixelFormat format() const noexcept override { return _format; }

    VideoResult updateFromFrame(const VideoFrame& frame) override;

    const uint8_t* rgba8Data() const noexcept override;
    uint32_t rgba8ByteSize() const noexcept override;

    bool isDirty() const noexcept override { return _dirty; }
    void clearDirty() noexcept override { _dirty = false; }

    // Opaque TextureHandle.id for hosts that avoid including this header
    // via IVideoFrameTexture::gpuTextureId().
    uint64_t gpuTextureId() const noexcept override;

    ayt::render::TextureHandle gpuHandle() const noexcept { return _handle; }

private:
    void destroyGpu() noexcept;

    ayt::render::Renderer& _renderer;
    ayt::render::TextureHandle _handle{};
    uint32_t _width = 0;
    uint32_t _height = 0;
    VideoPixelFormat _format = VideoPixelFormat::Unknown;
    std::vector<uint8_t> _rgba;
    bool _dirty = false;
};

} // namespace ayt::video
