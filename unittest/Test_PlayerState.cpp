// Test_PlayerState.cpp — V0.5 stub.
//
// Asserts the player state machine (design.md §10.4 transition table)
// + control surface: lifecycle open/play/pause/seek/stop, invalid
// transitions return InvalidState and leave state unchanged, loop/rate
// getters, media info availability per state.

#include <memory>

#include "AYTest.h"
#include "AYVideoPlayer.h"
#include "AYVideoTypes.h"
#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"

using namespace ayt::video;

namespace
{

// Player bound to Mock backends so interaction counters are observable.
// The player owns the backends (injected unique_ptrs); the accessors
// downcast the seams (AYVideoPlayer::demuxer()/decoder()) for counter
// assertions. NOTE: never hold the moved-from unique_ptr members —
// they are null after the move (that was a null-deref segfault).
struct MockPlayerFixture
{
    MockPlayerFixture(int32_t packetCount, int32_t frameCount)
        : player(std::make_unique<MockDemuxer>(packetCount),
                 std::make_unique<MockDecoder>(frameCount))
    {
    }
    MockDemuxer* demuxer() const
    {
        return static_cast<MockDemuxer*>(player.demuxer());
    }
    MockDecoder* decoder() const
    {
        return static_cast<MockDecoder*>(player.decoder());
    }
    AYVideoPlayer player;
};

} // namespace

TEST_SUITE(PlayerStateSuite)

    TEST_CASE(PlayerStartsIdle) {
        AYVideoPlayer p;
        CHECK_INT_EQ(static_cast<int>(p.state()),
                     static_cast<int>(PlayerState::Idle));
        CHECK_FALSE(p.isPlaying());
        CHECK_INT_EQ(static_cast<int>(p.lastResult()),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(PlayerOpenLifecycle) {
        // Mock backends so getMediaInfo reports a real (synthetic) stream;
        // the Null default pair reports a zeroed MediaInfo instead.
        AYVideoPlayer p(makeMockDemuxer(1), makeMockDecoder(1));
        // Operations before open are invalid.
        CHECK_INT_EQ(static_cast<int>(p.play()),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_INT_EQ(static_cast<int>(p.pause()),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_INT_EQ(static_cast<int>(p.stop()),
                     static_cast<int>(VideoResult::InvalidState));
        MediaInfo info;
        CHECK_INT_EQ(static_cast<int>(p.getMediaInfo(info)),
                     static_cast<int>(VideoResult::InvalidState));

        CHECK_INT_EQ(static_cast<int>(p.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(p.state()),
                     static_cast<int>(PlayerState::Ready));
        CHECK_INT_EQ(static_cast<int>(p.getMediaInfo(info)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(info.hasVideo);
    }

    TEST_CASE(PlayerPlayPauseSeekCycle) {
        // 100 frames >> queue depth (4): while paused the decode thread
        // blocks on backpressure (queue full) and can never reach EOS,
        // so the "resume from paused position" path (which seeks) is not
        // exercised here and the seek counter below is deterministic.
        MockPlayerFixture fx(100, 100);
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));

        CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(fx.player.isPlaying());

        CHECK_INT_EQ(static_cast<int>(fx.player.pause()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FALSE(fx.player.isPlaying());

        CHECK_INT_EQ(static_cast<int>(fx.player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(fx.player.isPlaying());

        // V1: play() from Ready replays from 0 (demuxer seek #1); the
        // explicit seek is #2. Both are issued on the player thread after
        // the decode loop was joined, so the counters are race-free.
        CHECK_INT_EQ(static_cast<int>(fx.player.seek(ayt::time::Duration::fromMs(80))),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.demuxer()->seekCount()), 2u);
        CHECK_TRUE(fx.player.isPlaying());
    }

    TEST_CASE(PlayerStopClosesBackends) {
        MockPlayerFixture fx(2, 2);
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.stop()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.state()),
                     static_cast<int>(PlayerState::Stopped));
        CHECK_TRUE(fx.demuxer()->wasClosed());
        CHECK_TRUE(fx.decoder()->wasClosed());
        CHECK_INT_EQ(static_cast<int>(fx.demuxer()->openCount()), 1u);
        CHECK_INT_EQ(static_cast<int>(fx.decoder()->openCount()), 1u);
    }

    TEST_CASE(PlayerCanReopenAfterStop) {
        MockPlayerFixture fx(2, 2);
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.stop()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://clip2")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.state()),
                     static_cast<int>(PlayerState::Ready));
        CHECK_INT_EQ(static_cast<int>(fx.demuxer()->openCount()), 2u);
    }

    TEST_CASE(PlayerOpenTwiceIsInvalidState) {
        MockPlayerFixture fx(2, 2);
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(fx.player.open("mock://again")),
                     static_cast<int>(VideoResult::InvalidState));
        // State unchanged after the rejected call.
        CHECK_INT_EQ(static_cast<int>(fx.player.state()),
                     static_cast<int>(PlayerState::Ready));
        CHECK_INT_EQ(static_cast<int>(fx.demuxer()->openCount()), 1u);
    }

    TEST_CASE(PlayerSeekFromIdleIsInvalidState) {
        AYVideoPlayer p;
        CHECK_INT_EQ(static_cast<int>(p.seek(ayt::time::Duration::fromMs(100))),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_INT_EQ(static_cast<int>(p.state()),
                     static_cast<int>(PlayerState::Idle));
    }

    TEST_CASE(PlayerLoopAndRateControl) {
        MockPlayerFixture fx(2, 2);
        CHECK_FALSE(fx.player.isLooping());
        fx.player.setLoop(true);
        CHECK_TRUE(fx.player.isLooping());

        CHECK_FLOAT_EQ(static_cast<float>(fx.player.rate()), 1.0f, 1e-9f);
        CHECK_INT_EQ(static_cast<int>(fx.player.setRate(2.0)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FLOAT_EQ(static_cast<float>(fx.player.rate()), 2.0f, 1e-9f);
        // Out-of-range rates are rejected, not clamped (design.md §9.3).
        CHECK_INT_EQ(static_cast<int>(fx.player.setRate(0.1)),
                     static_cast<int>(VideoResult::InvalidArgument));
        CHECK_FLOAT_EQ(static_cast<float>(fx.player.rate()), 2.0f, 1e-9f);
    }

    TEST_CASE(PlayerFailedStateRecoversViaStop) {
        // Fault injection: a backend that fails open drives the player
        // to Failed (design.md §10.4); stop() recovers to Stopped.
        auto failingDemuxer = makeMockDemuxer(1);
        auto* raw = static_cast<MockDemuxer*>(failingDemuxer.get());
        raw->setFailOpen(true);

        AYVideoPlayer p(std::move(failingDemuxer), makeNullDecoder());
        CHECK_INT_EQ(static_cast<int>(p.open("mock://broken")),
                     static_cast<int>(VideoResult::DemuxError));
        CHECK_INT_EQ(static_cast<int>(p.state()),
                     static_cast<int>(PlayerState::Failed));
        CHECK_INT_EQ(static_cast<int>(p.lastResult()),
                     static_cast<int>(VideoResult::DemuxError));

        // Failed state: play/pause/seek are all rejected...
        CHECK_INT_EQ(static_cast<int>(p.play()),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_INT_EQ(static_cast<int>(p.seek(ayt::time::Duration::fromMs(0))),
                     static_cast<int>(VideoResult::InvalidState));
        // ...but stop() recovers.
        CHECK_INT_EQ(static_cast<int>(p.stop()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(p.state()),
                     static_cast<int>(PlayerState::Stopped));
    }

TEST_SUITE_END
