// Test_AudioBridge.cpp — V2 AYAudio PCM bridge smoke (design.md §11).
//
// Attaches a Null AudioEngine, opens a video-only clip (no AAC path —
// A-14), verifies attach/detach contracts when Idle/Stopped.

#include "AYTest.h"
#include "AYVideoPlayer.h"
#include "AYVideoTypes.h"
#include "FFmpegTestMedia.h"
#include "IAYVideoBackendFactory.h"

#include <AYAudioEngine.h>
#include <AYAudioTypes.h>

using namespace ayt::video;
using namespace ayt::audio;
using namespace ayt::testmedia;

namespace
{

AudioSettings nullSettings()
{
    AudioSettings s{};
    s.backend = AudioBackendKind::Null;
    s.sampleRate = 48000;
    s.channels = 2;
    s.commandQueueCapacity = 64;
    s.maxVoices = 8;
    s.maxClips = 8;
    return s;
}

} // namespace

TEST_SUITE(AudioBridgeSuite)

    TEST_CASE(AttachAudioEngineOnlyWhenIdleOrStopped) {
        AudioEngine engine;
        CHECK(engine.initialize(nullSettings()));

        AYVideoPlayer player(makeFFmpegDemuxer(), makeFFmpegDecoder());
        CHECK_INT_EQ(static_cast<int>(player.attachAudioEngine(&engine)),
                     static_cast<int>(VideoResult::Ok));

        auto clip = makeClip(false);
        CHECK_INT_EQ(static_cast<int>(player.open(clip.path)),
                     static_cast<int>(VideoResult::Ok));
        // attach while Ready is rejected.
        CHECK_INT_EQ(static_cast<int>(player.attachAudioEngine(&engine)),
                     static_cast<int>(VideoResult::InvalidState));

        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.attachAudioEngine(nullptr)),
                     static_cast<int>(VideoResult::Ok));
        engine.shutdown();
    }

    TEST_CASE(VideoOnlyPlaybackWithAudioEngineAttached) {
        AudioEngine engine;
        CHECK(engine.initialize(nullSettings()));

        AYVideoPlayer player(makeFFmpegDemuxer(), makeFFmpegDecoder());
        CHECK_INT_EQ(static_cast<int>(player.attachAudioEngine(&engine)),
                     static_cast<int>(VideoResult::Ok));

        auto clip = makeClip(false);
        CHECK_INT_EQ(static_cast<int>(player.open(clip.path)),
                     static_cast<int>(VideoResult::Ok));
        MediaInfo info{};
        CHECK_INT_EQ(static_cast<int>(player.getMediaInfo(info)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FALSE(info.hasAudio);

        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));

        bool sawFrame = false;
        VideoFrame f{};
        float scratch[128]{};
        for (int i = 0; i < 400; ++i)
        {
            engine.submitFrame(0.0f);
            engine.render(scratch, 64);
            const VideoResult r = player.pullFrame(f);
            if (r == VideoResult::Ok && f.data)
            {
                sawFrame = true;
                break;
            }
            if (r == VideoResult::EndOfStream)
            {
                break;
            }
            if (player.state() == PlayerState::Failed)
            {
                break;
            }
        }
        CHECK(sawFrame);
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
        engine.shutdown();
    }

    TEST_CASE(SetRateMirrorsAudioTimeScale) {
        AudioEngine engine;
        CHECK(engine.initialize(nullSettings()));
        AYVideoPlayer player(makeFFmpegDemuxer(), makeFFmpegDecoder());
        CHECK_INT_EQ(static_cast<int>(player.attachAudioEngine(&engine)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FLOAT_EQ(engine.timeScale(), 1.0f, 1e-5f);

        CHECK_INT_EQ(static_cast<int>(player.setRate(1.5)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FLOAT_EQ(static_cast<float>(player.rate()), 1.5f, 1e-5f);
        CHECK_FLOAT_EQ(engine.timeScale(), 1.5f, 1e-5f);

        CHECK_INT_EQ(static_cast<int>(player.setRate(0.5)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FLOAT_EQ(engine.timeScale(), 0.5f, 1e-5f);

        // Attach after rate set should pick up current rate.
        CHECK_INT_EQ(static_cast<int>(player.attachAudioEngine(nullptr)),
                     static_cast<int>(VideoResult::Ok));
        engine.setTimeScale(1.0f);
        CHECK_INT_EQ(static_cast<int>(player.setRate(2.0)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.attachAudioEngine(&engine)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FLOAT_EQ(engine.timeScale(), 2.0f, 1e-5f);

        auto stats = player.queueStats();
        CHECK_INT_EQ(static_cast<int>(stats.audioStreamUnderruns), 0);
        engine.shutdown();
    }

TEST_SUITE_END
