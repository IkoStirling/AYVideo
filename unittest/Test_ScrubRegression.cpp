// Test_ScrubRegression.cpp — scrub → play intent contract (Mock).
//
// Covers the high-leverage scrub races that used to livelock the Demo:
//   * Scrub drag + play() CatchUp (ENDTRACK resume equivalent)
//   * Small backward twitch → extend (no extra demux seek)
//   * Open A → play → stop → open B → play

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

VideoResult pullWithRetry(AYVideoPlayer& p, VideoFrame& out, int maxTries = 3000)
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

struct ScrubFixture
{
    explicit ScrubFixture(int32_t packets = 40)
        : demuxRaw(nullptr)
        , player(makePlayer(packets))
    {
        FakeNow::us = 0;
    }

    AYVideoPlayer makePlayer(int32_t packets)
    {
        auto demux = std::make_unique<MockDemuxer>(packets);
        demuxRaw = demux.get();
        return AYVideoPlayer(std::move(demux),
                             std::make_unique<MockDecoder>(packets),
                             &FakeNow::tick);
    }

    MockDemuxer* demuxRaw;
    AYVideoPlayer player;
};

} // namespace

TEST_SUITE(ScrubRegressionSuite)

    TEST_CASE(ScrubPlayCatchUpPresentsNearThumb) {
        // Demo ENDTRACK+resume: Scrub to mid clip, then play() without a
        // second Scrub seek — CatchUp must present near the thumb floor.
        ScrubFixture fx(40);
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://scrub-a")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                     static_cast<int>(VideoResult::Ok));
        VideoFrame f{};
        CHECK_INT_EQ(static_cast<int>(pullWithRetry(fx.player, f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(f.data != nullptr);

        CHECK_INT_EQ(static_cast<int>(fx.player.pause()),
                     static_cast<int>(VideoResult::Ok));
        const auto thumb = ayt::time::Duration::fromUs(400'000); // 10 frames
        const uint32_t seeksBefore = fx.demuxRaw->seekCount();
        CHECK_INT_EQ(static_cast<int>(fx.player.seek(
                         thumb, AYVideoPlayer::SeekMode::Scrub)),
                     static_cast<int>(VideoResult::Ok));
        // Scrub posts in-loop with waitApplied=false — demux seek runs on
        // the decode thread. Poll briefly instead of asserting immediately
        // (that raced and flaked when the UI thread won).
        bool demuxSeeked = false;
        for (int i = 0; i < 200; ++i)
        {
            if (fx.demuxRaw->seekCount() > seeksBefore)
            {
                demuxSeeked = true;
                break;
            }
            fx.player.pollSeekPreview();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_TRUE(demuxSeeked);

        // Live scrub preview drain (bounded).
        for (int i = 0; i < 80; ++i)
        {
            fx.player.pollSeekPreview();
            VideoFrame cur{};
            if (fx.player.currentFrame(cur) == VideoResult::Ok && cur.data
                && cur.pts.toUs() + 200'000 >= thumb.toUs())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(fx.player.isPlaying());

        FakeNow::us += 40'000;
        const VideoResult r = pullWithRetry(fx.player, f, 4000);
        CHECK_INT_EQ(static_cast<int>(r), static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(f.data != nullptr);
        // Near-floor: within 0.5 s of thumb (CatchUp / held window).
        const std::int64_t d = (f.pts.toUs() >= thumb.toUs())
                                   ? (f.pts.toUs() - thumb.toUs())
                                   : (thumb.toUs() - f.pts.toUs());
        CHECK_TRUE(d <= 500'000);
        CHECK_INT_EQ(static_cast<int>(fx.player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(ScrubSmallBackwardTwitchExtendsWithoutReseek) {
        ScrubFixture fx(40);
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://scrub-twitch")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                     static_cast<int>(VideoResult::Ok));
        VideoFrame f{};
        (void)pullWithRetry(fx.player, f);
        CHECK_INT_EQ(static_cast<int>(fx.player.pause()),
                     static_cast<int>(VideoResult::Ok));

        const auto forward = ayt::time::Duration::fromUs(480'000);
        CHECK_INT_EQ(static_cast<int>(fx.player.seek(
                         forward, AYVideoPlayer::SeekMode::Scrub)),
                     static_cast<int>(VideoResult::Ok));
        // Let scrub walk produce lastOut near the ceiling.
        for (int i = 0; i < 100; ++i)
        {
            fx.player.pollSeekPreview();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            VideoFrame cur{};
            if (fx.player.currentFrame(cur) == VideoResult::Ok && cur.data
                && cur.pts.toUs() + 80'000 >= forward.toUs())
            {
                break;
            }
        }

        const uint32_t seeksAtCeil = fx.demuxRaw->seekCount();
        // Small back (~200 ms) within 0.5 s of lastOut → extend, no demux.
        const auto twitch = ayt::time::Duration::fromUs(320'000);
        CHECK_INT_EQ(static_cast<int>(fx.player.seek(
                         twitch, AYVideoPlayer::SeekMode::Scrub)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.demuxRaw->seekCount()),
                     static_cast<int>(seeksAtCeil));
        CHECK_INT_EQ(static_cast<int>(fx.player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(OpenFileAThenBPlaysCleanly) {
        ScrubFixture fx(24);
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://file-a")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                     static_cast<int>(VideoResult::Ok));
        VideoFrame f{};
        CHECK_INT_EQ(static_cast<int>(pullWithRetry(fx.player, f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(f.data != nullptr);
        CHECK_INT_EQ(static_cast<int>(fx.player.stop()),
                     static_cast<int>(VideoResult::Ok));

        // New Mock backends (simulates opening a second file).
        auto demuxB = std::make_unique<MockDemuxer>(24);
        MockDemuxer* demuxBRaw = demuxB.get();
        AYVideoPlayer playerB(std::move(demuxB),
                              std::make_unique<MockDecoder>(24),
                              &FakeNow::tick);
        FakeNow::us = 0;
        CHECK_INT_EQ(static_cast<int>(playerB.open("mock://file-b")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(playerB.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(pullWithRetry(playerB, f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(f.data != nullptr);
        CHECK_INT_EQ(static_cast<int>(f.pts.toUs()), 0);
        CHECK_TRUE(demuxBRaw->openCount() >= 1u);
        CHECK_INT_EQ(static_cast<int>(playerB.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(ScrubRegressionThreeRoundSoak) {
        // Lightweight soak: three consecutive CatchUp commits must stay green.
        for (int round = 0; round < 3; ++round)
        {
            ScrubFixture fx(32);
            CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://soak")),
                         static_cast<int>(VideoResult::Ok));
            CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                         static_cast<int>(VideoResult::Ok));
            VideoFrame f{};
            (void)pullWithRetry(fx.player, f);
            CHECK_INT_EQ(static_cast<int>(fx.player.pause()),
                         static_cast<int>(VideoResult::Ok));
            CHECK_INT_EQ(static_cast<int>(fx.player.seek(
                             ayt::time::Duration::fromUs(240'000),
                             AYVideoPlayer::SeekMode::Scrub)),
                         static_cast<int>(VideoResult::Ok));
            for (int i = 0; i < 40; ++i)
            {
                fx.player.pollSeekPreview();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                         static_cast<int>(VideoResult::Ok));
            FakeNow::us += 80'000;
            CHECK_INT_EQ(static_cast<int>(pullWithRetry(fx.player, f, 4000)),
                         static_cast<int>(VideoResult::Ok));
            CHECK_TRUE(f.data != nullptr);
            CHECK_INT_EQ(static_cast<int>(fx.player.stop()),
                         static_cast<int>(VideoResult::Ok));
        }
    }

TEST_SUITE_END
