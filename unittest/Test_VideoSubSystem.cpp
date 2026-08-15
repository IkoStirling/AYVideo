// Test_VideoSubSystem.cpp — V2+ GameLoop subsystem smoke (design.md §15).

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include "AYTest.h"
#include "AYVideo/VideoSubSystem.h"
#include "AYVideo/VideoTypes.h"
#include "AYVideo/IVideoBackendFactory.h"
#include "AYVideo/IVideoFrameSink.h"
#include "VideoComponent.h"
#include "backend/MockDecoder.h"
#include "backend/MockDemuxer.h"
#include "backend/MockVideoFrameTexture.h"

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

struct CountingSink : IAYVideoFrameSink
{
    uint32_t count = 0;
    void onVideoFrame(uint32_t /*id*/, IVideoFrameTexture& tex) override
    {
        ++count;
        CHECK(tex.rgba8Data() != nullptr);
    }
};

} // namespace

TEST_SUITE(VideoSubSystemSuite)

    TEST_CASE(VideoSubSystemPlayUpdateStopWithMockBackends) {
        VideoSubSystem sys;
        sys.setNowFn(&FakeNow::tick);
        sys.setBackendFactory([] {
            return std::make_pair(
                std::unique_ptr<IAYVideoDemuxer>(std::make_unique<MockDemuxer>(12)),
                std::unique_ptr<IAYVideoDecoder>(std::make_unique<MockDecoder>(12)));
        });
        CHECK(sys.initialize());
        CHECK(VideoSubSystem::findRegistered() == &sys);

        FakeNow::us = 0;
        const VideoPlaybackId id = sys.play("mock://clip", false);
        CHECK(id != InvalidVideoPlayback);
        CHECK(sys.player(id) != nullptr);
        CHECK(sys.player(id)->isPlaying());

        bool saw = false;
        for (int i = 0; i < 2000; ++i)
        {
            // Keep clock at 0 so the first Mock frame (pts=0) is due as
            // soon as the decode thread fills the queue.
            sys.update(0.0f);
            VideoFrame f{};
            if (sys.lastFrame(id, f) && f.data)
            {
                saw = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(saw);

        const VideoPlaybackInfo inf = sys.info(id);
        CHECK_INT_EQ(static_cast<int>(inf.state),
                     static_cast<int>(PlayerState::Playing));
        CHECK(inf.hasFrame);

        VideoComponent comp{};
        comp.playbackId = id;
        comp.currentPosition = inf.position;
        CHECK_INT_EQ(static_cast<int>(comp.playbackId), static_cast<int>(id));

        sys.stop(id);
        CHECK(sys.player(id) == nullptr);
        sys.shutdown();
    }

    TEST_CASE(VideoSubSystemUpdatesFrameTextureAndSink) {
        VideoSubSystem sys;
        sys.setNowFn(&FakeNow::tick);
        sys.setBackendFactory([] {
            return std::make_pair(
                std::unique_ptr<IAYVideoDemuxer>(std::make_unique<MockDemuxer>(8)),
                std::unique_ptr<IAYVideoDecoder>(std::make_unique<MockDecoder>(8)));
        });
        CHECK(sys.initialize());

        CountingSink sink;
        sys.setFrameSink(&sink);
        FakeNow::us = 0;
        const VideoPlaybackId id = sys.play("mock://tex", false);
        CHECK(id != InvalidVideoPlayback);

        MockVideoFrameTexture tex;
        sys.setFrameTexture(id, &tex);

        for (int i = 0; i < 2000; ++i)
        {
            sys.update(0.0f);
            if (tex.updateCount() > 0)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(tex.updateCount() > 0);
        CHECK(sink.count > 0);
        CHECK_INT_EQ(static_cast<int>(tex.format()),
                     static_cast<int>(VideoPixelFormat::RGBA8));

        sys.stop(id);
        sys.shutdown();
    }

    TEST_CASE(VideoSubSystemRejectsPlayBeforeInitialize) {
        VideoSubSystem sys;
        CHECK_INT_EQ(static_cast<int>(sys.play("mock://x")),
                     static_cast<int>(InvalidVideoPlayback));
    }

TEST_SUITE_END
