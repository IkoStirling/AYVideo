// Test_Frame.cpp — V0.5 stub.
//
// Asserts the data-carrier PODs from design.md §6.2 are well-formed:
// zeroed defaults, reference-semantics fields, pts typed as AYTime
// Duration.

#include <cstdint>

#include "AYTest.h"
#include "AYVideo/VideoFrame.h"
#include "AYVideo/VideoTypes.h"

using namespace ayt::video;

TEST_SUITE(FrameSuite)

    TEST_CASE(VideoPacketDefaultsAreZeroed) {
        VideoPacket p;
        CHECK_NULL(p.data);
        CHECK_INT_EQ(p.size, 0u);
        CHECK_FALSE(p.isVideo);
        CHECK_INT_EQ(p.streamIndex, -1);
        CHECK_INT_EQ(p.pts.toUs(), 0);
        CHECK_INT_EQ(p.dts.toUs(), 0);
    }

    TEST_CASE(VideoFrameDefaultsAreZeroed) {
        VideoFrame f;
        CHECK_NULL(f.data);
        CHECK_INT_EQ(f.dataSize, 0u);
        CHECK_INT_EQ(f.width, 0);
        CHECK_INT_EQ(f.height, 0);
        CHECK_INT_EQ(f.stride, 0u);
        CHECK_INT_EQ(static_cast<int>(f.format),
                     static_cast<int>(VideoPixelFormat::Unknown));
        CHECK_INT_EQ(f.pts.toUs(), 0);
    }

    TEST_CASE(VideoPacketPtsRoundTrip) {
        // pts must be typed as ayt::time::Duration (design.md §2.5
        // normative — AYTime value types are the module's time unit).
        VideoPacket p;
        p.pts = ayt::time::Duration::fromUs(1'234'567);
        CHECK_INT_EQ(p.pts.toUs(), 1'234'567);
    }

    TEST_CASE(VideoFrameStrideAndSizeRelationship) {
        // 320x240 RGBA8: dataSize must equal stride * height (the
        // Mock backend honors this — design.md §8.3).
        VideoFrame f;
        f.width = 320;
        f.height = 240;
        f.stride = 320 * 4;
        f.dataSize = f.stride * static_cast<uint32_t>(f.height);
        CHECK_INT_EQ(f.dataSize, 320u * 240u * 4u);
    }

TEST_SUITE_END
