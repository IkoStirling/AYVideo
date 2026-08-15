// Test_FrameStep.cpp — editor frame-step (stepFrames ±1) while Paused.

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

TEST_SUITE(FrameStepSuite)

    TEST_CASE(StepForwardAdvancesOneFrameWhilePaused) {
        FakeNow::us = 0;
        AYVideoPlayer player(std::make_unique<MockDemuxer>(30),
                             std::make_unique<MockDecoder>(30),
                             &FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(player.open("mock://step")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        VideoFrame f{};
        CHECK_INT_EQ(static_cast<int>(pullWithRetry(player, f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(f.data != nullptr);
        const std::int64_t firstPts = f.pts.toUs();

        // Advance clock so a couple of frames present, then pause.
        FakeNow::us += 80'000;
        (void)pullWithRetry(player, f);
        CHECK_INT_EQ(static_cast<int>(player.pause()),
                     static_cast<int>(VideoResult::Ok));

        VideoFrame before{};
        CHECK_INT_EQ(static_cast<int>(player.currentFrame(before)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(before.data != nullptr);
        const std::int64_t pausedPts = before.pts.toUs();

        CHECK_INT_EQ(static_cast<int>(player.stepFrames(1)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.state()),
                     static_cast<int>(PlayerState::Paused));
        VideoFrame after{};
        CHECK_INT_EQ(static_cast<int>(player.currentFrame(after)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(after.data != nullptr);
        CHECK_TRUE(after.pts.toUs() > pausedPts);
        // Nominal 25 fps → ~40 ms step (allow ±1 frame slack).
        const std::int64_t d = after.pts.toUs() - pausedPts;
        CHECK_TRUE(d >= 20'000 && d <= 80'000);
        CHECK_TRUE(player.position().toUs() == after.pts.toUs()
                   || (player.position().toUs() >= after.pts.toUs() - 1
                       && player.position().toUs()
                              <= after.pts.toUs() + 1));
        (void)firstPts;
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(StepBackwardSeeksNearPreviousFrame) {
        FakeNow::us = 0;
        AYVideoPlayer player(std::make_unique<MockDemuxer>(40),
                             std::make_unique<MockDecoder>(40),
                             &FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(player.open("mock://step-back")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        VideoFrame f{};
        FakeNow::us += 200'000;
        for (int i = 0; i < 8; ++i)
        {
            FakeNow::us += 40'000;
            (void)pullWithRetry(player, f, 500);
        }
        CHECK_INT_EQ(static_cast<int>(player.pause()),
                     static_cast<int>(VideoResult::Ok));
        VideoFrame before{};
        CHECK_INT_EQ(static_cast<int>(player.currentFrame(before)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(before.data != nullptr);
        const std::int64_t pausedPts = before.pts.toUs();
        CHECK_TRUE(pausedPts >= 80'000);

        CHECK_INT_EQ(static_cast<int>(player.stepFrames(-1)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.state()),
                     static_cast<int>(PlayerState::Paused));
        VideoFrame after{};
        CHECK_INT_EQ(static_cast<int>(player.currentFrame(after)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(after.data != nullptr);
        CHECK_TRUE(after.pts.toUs() < pausedPts);
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(StepRejectsInvalidDeltaAndIdle) {
        AYVideoPlayer player(std::make_unique<MockDemuxer>(4),
                             std::make_unique<MockDecoder>(4));
        CHECK_INT_EQ(static_cast<int>(player.stepFrames(1)),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_INT_EQ(static_cast<int>(player.open("mock://step-args")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.pause()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.stepFrames(0)),
                     static_cast<int>(VideoResult::InvalidArgument));
        CHECK_INT_EQ(static_cast<int>(player.stepFrames(2)),
                     static_cast<int>(VideoResult::InvalidArgument));
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

TEST_SUITE_END
