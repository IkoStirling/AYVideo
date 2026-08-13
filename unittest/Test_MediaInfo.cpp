// Test_MediaInfo.cpp — V0.5 stub.
//
// Asserts the MediaInfo POD (design.md §6.1) is well-formed: zeroed
// defaults, copy semantics, field assignment.

#include "AYTest.h"
#include "AYVideoMediaInfo.h"

using namespace ayt::video;

TEST_SUITE(MediaInfoSuite)

    TEST_CASE(MediaInfoDefaultsAreZeroed) {
        // design.md §6.1: value type with 0/empty defaults = "unknown"
        // states are distinguishable from real values.
        MediaInfo info;
        CHECK_INT_EQ(info.width, 0);
        CHECK_INT_EQ(info.height, 0);
        CHECK_FLOAT_EQ(static_cast<float>(info.frameRate), 0.0f, 1e-9f);
        CHECK_FLOAT_EQ(static_cast<float>(info.durationSec), 0.0f, 1e-9f);
        CHECK_FALSE(info.hasVideo);
        CHECK_FALSE(info.hasAudio);
        CHECK_TRUE(info.videoCodec.empty());
        CHECK_TRUE(info.audioCodec.empty());
    }

    TEST_CASE(MediaInfoAssignmentRoundTrip) {
        MediaInfo a;
        a.width = 1920;
        a.height = 1080;
        a.frameRate = 60.0;
        a.durationSec = 90.0;
        a.hasVideo = true;
        a.hasAudio = true;
        a.videoCodec = "h264";
        a.audioCodec = "aac";

        MediaInfo b = a;
        CHECK_INT_EQ(b.width, 1920);
        CHECK_INT_EQ(b.height, 1080);
        CHECK_FLOAT_EQ(static_cast<float>(b.frameRate), 60.0f, 1e-9f);
        CHECK_FLOAT_EQ(static_cast<float>(b.durationSec), 90.0f, 1e-9f);
        CHECK_TRUE(b.hasVideo);
        CHECK_TRUE(b.hasAudio);
        CHECK_TRUE(b.videoCodec == "h264");
        CHECK_TRUE(b.audioCodec == "aac");
    }

TEST_SUITE_END
