// Test_RendererFrameTexture.cpp — V3 GPU IVideoFrameTexture bridge (§12).

#include <cstdint>
#include <cstring>
#include <vector>

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYVideoFrame.h"
#include "AYVideoTypes.h"
#include "IAYVideoBackendFactory.h"
#include "IVideoFrameTexture.h"
#include "backend/RendererVideoFrameTexture.h"

using namespace ayt::video;
using ayt::render::Backend;
using ayt::render::InitDesc;
using ayt::render::Renderer;

namespace
{

VideoFrame makeRgbaSolid(std::vector<uint8_t>& storage,
                         int32_t w, int32_t h,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    storage.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (size_t i = 0; i < storage.size(); i += 4)
    {
        storage[i + 0] = r;
        storage[i + 1] = g;
        storage[i + 2] = b;
        storage[i + 3] = a;
    }
    VideoFrame f{};
    f.data = storage.data();
    f.dataSize = static_cast<uint32_t>(storage.size());
    f.width = w;
    f.height = h;
    f.stride = static_cast<uint32_t>(w) * 4u;
    f.format = VideoPixelFormat::RGBA8;
    return f;
}

} // namespace

TEST_SUITE(RendererFrameTextureSuite)

    TEST_CASE(FactoryUploadsDynamicTexture) {
        Renderer renderer;
        InitDesc desc;
        desc.backend = Backend::Noop;
        desc.width = 64;
        desc.height = 64;
        CHECK(renderer.initialize(desc));

        auto tex = makeRendererVideoFrameTexture(renderer);
        CHECK(tex != nullptr);
        CHECK_INT_EQ(static_cast<int>(tex->gpuTextureId()), 0);

        std::vector<uint8_t> pixels;
        VideoFrame frame = makeRgbaSolid(pixels, 8, 4, 10, 20, 30, 255);
        CHECK_INT_EQ(static_cast<int>(tex->updateFromFrame(frame)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(tex->isDirty());
        CHECK(tex->gpuTextureId() != 0);
        CHECK_INT_EQ(static_cast<int>(tex->width()), 8);
        CHECK_INT_EQ(static_cast<int>(tex->height()), 4);
        CHECK_INT_EQ(static_cast<int>(tex->format()),
                     static_cast<int>(VideoPixelFormat::RGBA8));
        CHECK(tex->rgba8Data() != nullptr);
        CHECK_INT_EQ(static_cast<int>(tex->rgba8ByteSize()), 8 * 4 * 4);

        auto* gpu = dynamic_cast<RendererVideoFrameTexture*>(tex.get());
        CHECK(gpu != nullptr);
        CHECK(gpu->gpuHandle().isValid());

        // Second update same size reuses the handle.
        const uint64_t id = tex->gpuTextureId();
        frame = makeRgbaSolid(pixels, 8, 4, 40, 50, 60, 255);
        CHECK_INT_EQ(static_cast<int>(tex->updateFromFrame(frame)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(tex->gpuTextureId() == id);

        // Resize recreates.
        frame = makeRgbaSolid(pixels, 16, 8, 1, 2, 3, 255);
        CHECK_INT_EQ(static_cast<int>(tex->updateFromFrame(frame)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(tex->gpuTextureId() != id);
        CHECK_INT_EQ(static_cast<int>(tex->width()), 16);
        CHECK_INT_EQ(static_cast<int>(tex->height()), 8);

        tex->clearDirty();
        CHECK(!tex->isDirty());
        tex.reset();
        renderer.shutdown();
    }

    TEST_CASE(CpuTextureGpuIdStaysZero) {
        auto tex = makeCpuVideoFrameTexture();
        std::vector<uint8_t> pixels;
        VideoFrame frame = makeRgbaSolid(pixels, 2, 2, 7, 8, 9, 255);
        CHECK_INT_EQ(static_cast<int>(tex->updateFromFrame(frame)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(tex->gpuTextureId()), 0);
    }

TEST_SUITE_END
