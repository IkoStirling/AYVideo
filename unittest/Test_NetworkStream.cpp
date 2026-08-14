// Test_NetworkStream.cpp — V5 HTTP progressive: buffering + reconnect.

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "AYTest.h"
#include "AYVideoPlayer.h"
#include "AYVideoTypes.h"
#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"

#include <aytime/TimePoint.h>

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

VideoResult pullWithRetry(AYVideoPlayer& p, VideoFrame& out, int maxTries = 4000)
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

bool waitUntilNotBuffering(AYVideoPlayer& p, int maxTries = 4000)
{
    for (int i = 0; i < maxTries; ++i)
    {
        VideoFrame f;
        (void)p.pullFrame(f);
        if (!p.isBuffering())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !p.isBuffering();
}

} // namespace

TEST_SUITE(NetworkStreamSuite)

    TEST_CASE(HttpOpenIsNotSeekable) {
        FakeNow::us = 0;
        AYVideoPlayer player(std::make_unique<MockDemuxer>(8),
                             std::make_unique<MockDecoder>(8), &FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(player.open("http://cdn.example/clip.mp4")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.state()),
                     static_cast<int>(PlayerState::Ready));
        CHECK_INT_EQ(static_cast<int>(
                         player.seek(ayt::time::Duration::fromUs(80'000))),
                     static_cast<int>(VideoResult::UnsupportedFormat));
        CHECK_INT_EQ(static_cast<int>(player.state()),
                     static_cast<int>(PlayerState::Ready));
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(HttpsOpenBufferingEvents) {
        FakeNow::us = 0;
        AYVideoPlayer player(std::make_unique<MockDemuxer>(16),
                             std::make_unique<MockDecoder>(16), &FakeNow::tick);
        player.setBufferWatermarks(0, 2);
        std::vector<bool> events;
        player.setOnBufferingChanged([&](bool b) { events.push_back(b); });

        CHECK_INT_EQ(static_cast<int>(player.open("https://cdn.example/a.mp4")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(player.isBuffering());
        CHECK_TRUE(!events.empty() && events.front() == true);

        CHECK_TRUE(waitUntilNotBuffering(player));
        CHECK_TRUE(!player.isBuffering());
        bool sawFalse = false;
        for (bool b : events)
        {
            if (!b)
            {
                sawFalse = true;
            }
        }
        CHECK_TRUE(sawFalse);

        FakeNow::us += 40'000;
        VideoFrame f;
        CHECK_INT_EQ(static_cast<int>(pullWithRetry(player, f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(f.data != nullptr);
        CHECK_INT_EQ(static_cast<int>(player.state()),
                     static_cast<int>(PlayerState::Playing));
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(ReconnectSucceedsAfterDisconnect) {
        FakeNow::us = 0;
        auto demux = std::make_unique<MockDemuxer>(12);
        auto* demuxRaw = demux.get();
        // Trip before the first packet so playback cannot succeed without
        // DecodeLoop reconnect (avoids racing past a late latch).
        demuxRaw->disconnectAfter(0);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(12),
                             &FakeNow::tick);
        player.setBufferWatermarks(0, 1);

        CHECK_INT_EQ(static_cast<int>(player.open("http://cdn.example/live.mp4")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(waitUntilNotBuffering(player));

        int got = 0;
        for (int i = 0; i < 40 && got < 6; ++i)
        {
            FakeNow::us += 40'000;
            VideoFrame f;
            const VideoResult r = pullWithRetry(player, f, 2000);
            if (r == VideoResult::Ok && f.data != nullptr)
            {
                ++got;
            }
            CHECK_INT_EQ(static_cast<int>(player.state()),
                         static_cast<int>(PlayerState::Playing));
        }
        CHECK_TRUE(got >= 6);
        CHECK_TRUE(demuxRaw->reconnectCount() >= 1);
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(ReconnectExhaustedGoesFailed) {
        FakeNow::us = 0;
        auto demux = std::make_unique<MockDemuxer>(20);
        auto* demuxRaw = demux.get();
        demuxRaw->disconnectAfter(1);
        demuxRaw->failNextReconnects(100);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(20),
                             &FakeNow::tick);
        player.setBufferWatermarks(0, 1);

        CHECK_INT_EQ(static_cast<int>(player.open("http://cdn.example/dead.mp4")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));

        PlayerState st = player.state();
        for (int i = 0; i < 8000 && st == PlayerState::Playing; ++i)
        {
            FakeNow::us += 40'000;
            VideoFrame f;
            (void)player.pullFrame(f);
            st = player.state();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_INT_EQ(static_cast<int>(st), static_cast<int>(PlayerState::Failed));
        CHECK_INT_EQ(static_cast<int>(player.lastResult()),
                     static_cast<int>(VideoResult::DemuxError));
        CHECK_TRUE(demuxRaw->reconnectCount() >= 3);
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(LocalFileStillSoftSkipsWithoutReconnect) {
        // Regression: local paths keep V4 soft-skip (reconnectMax=0).
        FakeNow::us = 0;
        auto demux = std::make_unique<MockDemuxer>(8);
        auto* demuxRaw = demux.get();
        demuxRaw->failReadAt(2);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(8),
                             &FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(player.open("mock://local")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(!player.isBuffering());

        int got = 0;
        for (int i = 0; i < 8 && got < 3; ++i)
        {
            FakeNow::us += 40'000;
            VideoFrame f;
            if (pullWithRetry(player, f) == VideoResult::Ok && f.data)
            {
                ++got;
            }
        }
        CHECK_TRUE(got >= 3);
        CHECK_INT_EQ(static_cast<int>(demuxRaw->reconnectCount()), 0);
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

TEST_SUITE_END
