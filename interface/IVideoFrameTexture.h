#pragma once
// IVideoFrameTexture.h — CPU/GPU frame texture seam (design.md §12).
//
// AYVideo owns the abstract surface; AYRenderer (or a mock) implements
// upload. Public headers stay FFmpeg/bgfx-free (G-01). updateFromFrame
// accepts any VideoPixelFormat the converter supports and stores an
// RGBA8 staging buffer for the upload bridge.

#include <AYVideoFrame.h>
#include <AYVideoTypes.h>

#include <cstdint>

namespace ayt::video
{

class IVideoFrameTexture
{
public:
    virtual ~IVideoFrameTexture() = default;

    virtual uint32_t width() const noexcept = 0;
    virtual uint32_t height() const noexcept = 0;

    // Staging format after a successful update (RGBA8 for the upload
    // bridge). Unknown before the first update.
    virtual VideoPixelFormat format() const noexcept = 0;

    // Converts `frame` to RGBA8 staging (I420/NV12/RGBA8/BGRA8) and
    // marks the texture dirty for the next GPU upload.
    //   Ok              — staging updated
    //   InvalidArgument — null/empty/unsupported geometry
    //   UnsupportedFormat — pixel format not convertible
    virtual VideoResult updateFromFrame(const VideoFrame& frame) = 0;

    // RGBA8 staging bytes (w*h*4). Valid until the next updateFromFrame
    // / destruction. nullptr before the first successful update.
    virtual const uint8_t* rgba8Data() const noexcept = 0;
    virtual uint32_t rgba8ByteSize() const noexcept = 0;

    // True after updateFromFrame until clearDirty() (GPU bridge clears
    // after upload).
    virtual bool isDirty() const noexcept = 0;
    virtual void clearDirty() noexcept = 0;

    // Opaque GPU texture id (ayt::render::TextureHandle::id). 0 for
    // CPU/Mock staging textures or before the first successful GPU
    // create. Keeps AYRenderer types out of this public header (G-01).
    virtual uint64_t gpuTextureId() const noexcept { return 0; }
};

} // namespace ayt::video
