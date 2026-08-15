// Test_DecodeAccel.cpp — V6 hardware-decode preference (Mock + soft path).

#include <memory>

#include "AYTest.h"
#include "AYVideo/VideoPlayer.h"
#include "AYVideo/VideoTypes.h"
#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"

#include <AYTime/TimePoint.h>

using namespace ayt::video;

namespace
{

struct FakeNow
{
    static std::int64_t us;
    static ayt::time::TimePoint tick() noexcept
    {
        return ayt::time::TimePoint::fromUnixUs(us);
    }
};
std::int64_t FakeNow::us = 0;

} // namespace

TEST_SUITE(DecodeAccelSuite)

    TEST_CASE(PlayerPassesPreferredAccelToMock) {
        FakeNow::us = 0;
        auto demux = std::make_unique<MockDemuxer>(4);
        auto decoder = std::make_unique<MockDecoder>(4);
        MockDecoder* decRaw = decoder.get();
        AYVideoPlayer player(std::move(demux), std::move(decoder),
                             &FakeNow::tick);
        player.setPreferredDecodeAccel(VideoDecodeAccel::Auto);
        CHECK_INT_EQ(static_cast<int>(player.preferredDecodeAccel()),
                     static_cast<int>(VideoDecodeAccel::Auto));
        CHECK_INT_EQ(static_cast<int>(player.open("mock://accel")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(
            static_cast<int>(decRaw->lastOpenParams().preferredAccel),
            static_cast<int>(VideoDecodeAccel::Auto));
        CHECK_TRUE(decRaw->lastOpenParams().allowSoftwareFallback);
        // Mock has no HW path — active stays software None.
        CHECK_INT_EQ(static_cast<int>(player.activeDecodeAccel()),
                     static_cast<int>(VideoDecodeAccel::None));
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(PreferredNoneForcesSoftOnMock) {
        FakeNow::us = 0;
        auto decoder = std::make_unique<MockDecoder>(2);
        MockDecoder* decRaw = decoder.get();
        AYVideoPlayer player(std::make_unique<MockDemuxer>(2),
                             std::move(decoder), &FakeNow::tick);
        player.setPreferredDecodeAccel(VideoDecodeAccel::None);
        CHECK_INT_EQ(static_cast<int>(player.open("mock://soft")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(
            static_cast<int>(decRaw->lastOpenParams().preferredAccel),
            static_cast<int>(VideoDecodeAccel::None));
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

TEST_SUITE_END
