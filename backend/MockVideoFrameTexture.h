#pragma once
// MockVideoFrameTexture.h — records updateFromFrame for tests (§12 / §19).

#include <AYVideo/IVideoFrameTexture.h>

#include <cstdint>
#include <vector>

namespace ayt::video
{

class MockVideoFrameTexture final : public IVideoFrameTexture
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

    uint32_t updateCount() const noexcept { return _updateCount; }
    VideoPixelFormat lastSourceFormat() const noexcept { return _lastSource; }

private:
    uint32_t _width = 0;
    uint32_t _height = 0;
    VideoPixelFormat _format = VideoPixelFormat::Unknown;
    VideoPixelFormat _lastSource = VideoPixelFormat::Unknown;
    std::vector<uint8_t> _rgba;
    bool _dirty = false;
    uint32_t _updateCount = 0;
};

} // namespace ayt::video
