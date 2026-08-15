// Test_FrameTexture.cpp — V3 color convert + IVideoFrameTexture (§12).

#include <cstdint>
#include <cstring>
#include <vector>

#include "AYTest.h"
#include "AYVideo/VideoFrame.h"
#include "AYVideo/VideoTypes.h"
#include "AYVideo/IVideoBackendFactory.h"
#include "AYVideo/IVideoFrameTexture.h"
#include "backend/MockVideoFrameTexture.h"
#include "src/VideoColorConvert.h"

using namespace ayt::video;

namespace
{

// Build a solid-ish I420 frame: Y=constant, U=V=128 (neutral chroma → grey).
VideoFrame makeI420Grey(std::vector<uint8_t>& storage,
                        int32_t w, int32_t h, uint8_t yValue)
{
    const uint32_t cw = static_cast<uint32_t>((w + 1) / 2);
    const uint32_t ch = static_cast<uint32_t>((h + 1) / 2);
    const size_t yBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t uBytes = static_cast<size_t>(cw) * ch;
    storage.resize(yBytes + uBytes + uBytes);
    std::memset(storage.data(), yValue, yBytes);
    std::memset(storage.data() + yBytes, 128, uBytes + uBytes);

    VideoFrame f{};
    f.data = storage.data();
    f.dataSize = static_cast<uint32_t>(storage.size());
    f.width = w;
    f.height = h;
    f.stride = static_cast<uint32_t>(w);
    f.format = VideoPixelFormat::I420;
    f.planeOffset[0] = 0;
    f.planeOffset[1] = static_cast<uint32_t>(yBytes);
    f.planeOffset[2] = static_cast<uint32_t>(yBytes + uBytes);
    return f;
}

} // namespace

TEST_SUITE(FrameTextureSuite)

    TEST_CASE(ConvertI420NeutralChromaToGreyRgba) {
        std::vector<uint8_t> storage;
        // Y=16 → black; Y=235 → white in limited range. Mid grey ~128.
        VideoFrame f = makeI420Grey(storage, 4, 4, 128);
        std::vector<uint8_t> rgba;
        CHECK_INT_EQ(static_cast<int>(convertFrameToRgba8(f, rgba)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(rgba.size()), 4 * 4 * 4);
        // Mid-grey limited-range Y≈128 → R/G/B roughly equal, non-zero.
        CHECK(rgba[0] > 40);
        CHECK_INT_EQ(static_cast<int>(rgba[0]), static_cast<int>(rgba[1]));
        CHECK_INT_EQ(static_cast<int>(rgba[1]), static_cast<int>(rgba[2]));
        CHECK_INT_EQ(static_cast<int>(rgba[3]), 255);
    }

    TEST_CASE(ConvertRgba8Passthrough) {
        std::vector<uint8_t> src = {10, 20, 30, 255, 40, 50, 60, 255};
        VideoFrame f{};
        f.data = src.data();
        f.dataSize = 8;
        f.width = 2;
        f.height = 1;
        f.format = VideoPixelFormat::RGBA8;
        std::vector<uint8_t> out;
        CHECK_INT_EQ(static_cast<int>(convertFrameToRgba8(f, out)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(out.size()), 8);
        CHECK_INT_EQ(static_cast<int>(out[0]), 10);
        CHECK_INT_EQ(static_cast<int>(out[4]), 40);
    }

    TEST_CASE(CpuTextureUpdateMarksDirty) {
        auto tex = makeCpuVideoFrameTexture();
        CHECK(tex != nullptr);
        std::vector<uint8_t> storage;
        VideoFrame f = makeI420Grey(storage, 8, 8, 200);
        CHECK_INT_EQ(static_cast<int>(tex->updateFromFrame(f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(tex->isDirty());
        CHECK_INT_EQ(static_cast<int>(tex->width()), 8);
        CHECK_INT_EQ(static_cast<int>(tex->height()), 8);
        CHECK_INT_EQ(static_cast<int>(tex->format()),
                     static_cast<int>(VideoPixelFormat::RGBA8));
        CHECK(tex->rgba8Data() != nullptr);
        CHECK_INT_EQ(static_cast<int>(tex->rgba8ByteSize()), 8 * 8 * 4);
        tex->clearDirty();
        CHECK_FALSE(tex->isDirty());
    }

    TEST_CASE(MockTextureCountsUpdates) {
        MockVideoFrameTexture mock;
        std::vector<uint8_t> px(4 * 4, 0x7F);
        VideoFrame f{};
        f.data = px.data();
        f.dataSize = 16; // too small for 4x4 RGBA — expect InvalidArgument
        f.width = 4;
        f.height = 4;
        f.format = VideoPixelFormat::RGBA8;
        CHECK_INT_EQ(static_cast<int>(mock.updateFromFrame(f)),
                     static_cast<int>(VideoResult::InvalidArgument));
        CHECK_INT_EQ(static_cast<int>(mock.updateCount()), 0);

        std::vector<uint8_t> ok(4 * 4 * 4, 0x11);
        f.data = ok.data();
        f.dataSize = static_cast<uint32_t>(ok.size());
        CHECK_INT_EQ(static_cast<int>(mock.updateFromFrame(f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(mock.updateCount()), 1);
        CHECK_INT_EQ(static_cast<int>(mock.lastSourceFormat()),
                     static_cast<int>(VideoPixelFormat::RGBA8));
    }

TEST_SUITE_END
