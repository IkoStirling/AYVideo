// Test_MemoryPressure.cpp — V4 FrameQueue DropOldest + bounded size.

#include <chrono>
#include <thread>
#include <vector>

#include "AYTest.h"
#include "AYVideoFrame.h"
#include "AYVideoTypes.h"
#include "src/FrameQueue.h"

using namespace ayt::video;

namespace
{

VideoFrame makeTinyRgba(std::vector<uint8_t>& storage, uint8_t tag)
{
    storage.assign(4, tag);
    VideoFrame f{};
    f.data = storage.data();
    f.dataSize = 4;
    f.width = 1;
    f.height = 1;
    f.stride = 4;
    f.format = VideoPixelFormat::RGBA8;
    return f;
}

} // namespace

TEST_SUITE(MemoryPressureSuite)

    TEST_CASE(FrameQueueDropOldestStaysBounded) {
        FrameQueue q(4); // rounds up to power-of-two capacity 4
        q.setOverflowPolicy(FrameQueueOverflowPolicy::DropOldest);
        CHECK_INT_EQ(static_cast<int>(q.overflowPolicy()),
                     static_cast<int>(FrameQueueOverflowPolicy::DropOldest));
        CHECK_INT_EQ(static_cast<int>(q.capacity()), 4);

        std::vector<uint8_t> px;
        for (int i = 0; i < 20; ++i)
        {
            VideoFrame f = makeTinyRgba(px, static_cast<uint8_t>(i));
            q.push(f);
            CHECK_TRUE(q.size() <= q.capacity());
        }
        CHECK_TRUE(q.dropped() >= 16);
        CHECK_INT_EQ(static_cast<int>(q.size()), 4);

        QueuedFrame out;
        CHECK(q.tryPop(out));
        // Oldest of the retained window: tags 16..19 after dropping 0..15
        CHECK_INT_EQ(static_cast<int>(out.pixels[0]), 16);
    }

    TEST_CASE(FrameQueueBlockDefaultDoesNotDrop) {
        FrameQueue q(2);
        CHECK_INT_EQ(static_cast<int>(q.overflowPolicy()),
                     static_cast<int>(FrameQueueOverflowPolicy::Block));
        std::vector<uint8_t> px;
        q.push(makeTinyRgba(px, 1));
        q.push(makeTinyRgba(px, 2));
        // Fill without consuming — a third push would block; instead
        // pop one and push again to prove Block still works.
        QueuedFrame out;
        CHECK(q.tryPop(out));
        q.push(makeTinyRgba(px, 3));
        CHECK_INT_EQ(static_cast<int>(q.dropped()), 0);
        CHECK_TRUE(q.size() <= q.capacity());
    }

TEST_SUITE_END
