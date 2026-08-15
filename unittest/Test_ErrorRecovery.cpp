// Test_ErrorRecovery.cpp — V4 soft-skip mid-stream errors (Mock only).

#include <chrono>
#include <memory>
#include <thread>

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

VideoResult pullWithRetry(AYVideoPlayer& p, VideoFrame& out, int maxTries = 2000)
{
    for (int i = 0; i < maxTries; ++i)
    {
        const VideoResult r = p.pullFrame(out);
        if (r != VideoResult::Ok || out.data != nullptr)
        {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return VideoResult::Ok;
}

} // namespace

TEST_SUITE(ErrorRecoverySuite)

    TEST_CASE(DemuxErrorMidStreamKeepsPlaying) {
        FakeNow::us = 0;
        auto demux = std::make_unique<MockDemuxer>(8);
        auto* demuxRaw = demux.get();
        demuxRaw->failReadAt(2);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(8),
                             &FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));

        int got = 0;
        for (int i = 0; i < 8 && got < 3; ++i)
        {
            FakeNow::us += 40'000;
            VideoFrame f;
            const VideoResult r = pullWithRetry(player, f);
            CHECK_INT_EQ(static_cast<int>(player.state()),
                         static_cast<int>(PlayerState::Playing));
            if (r == VideoResult::Ok && f.data != nullptr)
            {
                ++got;
            }
        }
        CHECK_TRUE(got >= 3);
        CHECK_INT_EQ(static_cast<int>(player.state()),
                     static_cast<int>(PlayerState::Playing));
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(DecodeErrorMidStreamKeepsPlaying) {
        FakeNow::us = 0;
        auto decoder = std::make_unique<MockDecoder>(8);
        auto* decoderRaw = decoder.get();
        decoderRaw->failFeedAt(2);
        AYVideoPlayer player(std::make_unique<MockDemuxer>(8), std::move(decoder),
                             &FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));

        int got = 0;
        for (int i = 0; i < 8 && got < 3; ++i)
        {
            FakeNow::us += 40'000;
            VideoFrame f;
            const VideoResult r = pullWithRetry(player, f);
            CHECK_INT_EQ(static_cast<int>(player.state()),
                         static_cast<int>(PlayerState::Playing));
            if (r == VideoResult::Ok && f.data != nullptr)
            {
                ++got;
            }
        }
        CHECK_TRUE(got >= 3);
        CHECK_INT_EQ(static_cast<int>(player.state()),
                     static_cast<int>(PlayerState::Playing));
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

TEST_SUITE_END
