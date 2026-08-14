#pragma once
// CpuVideoFrameTexture.h — RGBA8 staging texture (design.md §12).
//
// Owns a CPU RGBA8 buffer updated via convertFrameToRgba8. The AYRenderer
// upload bridge (follow-up) reads rgba8Data() when isDirty().

#include <IVideoFrameTexture.h>

#include <cstdint>
#include <vector>

namespace ayt::video
{

class CpuVideoFrameTexture final : public IVideoFrameTexture
{
public:
    uint32_t width() const noexcept override { return _width; }
    uint32_t height() const noexcept override { return _height; }
    VideoPixelFormat format() const noexcept override { return _format; }

    VideoResult updateFromFrame(const VideoFrame& frame) override;

    const uint8_t* rgba8Data() const noexcept override;
    uint32_t rgba8ByteSize() const noexcept override;

    bool isDirty() const noexcept override { return _dirty; }
    void clearDirty() noexcept override { _dirty = false; }

private:
    uint32_t _width = 0;
    uint32_t _height = 0;
    VideoPixelFormat _format = VideoPixelFormat::Unknown;
    std::vector<uint8_t> _rgba;
    bool _dirty = false;
};

} // namespace ayt::video
