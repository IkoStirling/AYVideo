// Test_SyncClock.cpp — V0.5 stub.
//
// Asserts the sync clock contract (design.md §9): engine-clock path
// with an injectable now-provider for determinism; position anchoring
// at reset; pause/resume continuity; rate rejection for out-of-range
// values; AudioMaster gated until V2.

#include <cstdint>

#include "AYTest.h"
#include "AYVideoSyncClock.h"
#include "AYVideoTypes.h"
#include <aytime/TimePoint.h>

using namespace ayt::video;

namespace
{

// Deterministic fake wall clock (design.md §19: tests must not depend
// on real wall time). NowFn signature is `TimePoint() noexcept`.
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

TEST_SUITE(SyncClockSuite)

    TEST_CASE(SyncClockDefaultsToEngineClock) {
        AYVideoSyncClock clk;
        CHECK_INT_EQ(static_cast<int>(clk.source()),
                     static_cast<int>(SyncSource::EngineClock));
        CHECK_FLOAT_EQ(static_cast<float>(clk.rate()), 1.0f, 1e-9f);
        CHECK_INT_EQ(clk.position().toUs(), 0);
    }

    TEST_CASE(SyncClockPositionAnchorsAtReset) {
        FakeNow::us = 0;
        AYVideoSyncClock clk(&FakeNow::tick);
        clk.reset(ayt::time::Duration::fromMs(500));
        CHECK_INT_EQ(clk.position().toUs(), 500'000);
    }

    TEST_CASE(SyncClockPositionAdvancesWithFakeNow) {
        FakeNow::us = 0;
        AYVideoSyncClock clk(&FakeNow::tick);
        clk.reset();
        CHECK_INT_EQ(clk.position().toUs(), 0);

        FakeNow::us = 250'000; // wall +250 ms, rate 1.0
        CHECK_INT_EQ(clk.position().toUs(), 250'000);
    }

    TEST_CASE(SyncClockPauseFreezesPosition) {
        FakeNow::us = 0;
        AYVideoSyncClock clk(&FakeNow::tick);
        clk.reset();

        FakeNow::us = 1'000'000; // wall +1 s
        CHECK_INT_EQ(clk.position().toUs(), 1'000'000);

        clk.markPaused();
        CHECK_INT_EQ(clk.position().toUs(), 1'000'000);

        // Wall keeps moving while paused — position must stay frozen.
        FakeNow::us = 2'000'000;
        CHECK_INT_EQ(clk.position().toUs(), 1'000'000);

        // Resume re-anchors: position continues from the frozen value.
        FakeNow::us = 3'000'000;
        clk.markResumed();
        CHECK_INT_EQ(clk.position().toUs(), 1'000'000);
        FakeNow::us = 3'500'000;
        CHECK_INT_EQ(clk.position().toUs(), 1'500'000);
    }

    TEST_CASE(SyncClockPositionHonorsRate) {
        FakeNow::us = 0;
        AYVideoSyncClock clk(&FakeNow::tick);
        clk.reset();
        CHECK_INT_EQ(static_cast<int>(clk.setRate(2.0)),
                     static_cast<int>(VideoResult::Ok));

        FakeNow::us = 1'000'000; // wall +1 s at rate 2.0 -> 2 s position
        CHECK_INT_EQ(clk.position().toUs(), 2'000'000);
    }

    TEST_CASE(SyncClockRejectsOutOfRangeRate) {
        AYVideoSyncClock clk(&FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(clk.setRate(0.1)),
                     static_cast<int>(VideoResult::InvalidArgument));
        CHECK_INT_EQ(static_cast<int>(clk.setRate(8.0)),
                     static_cast<int>(VideoResult::InvalidArgument));
        CHECK_FLOAT_EQ(static_cast<float>(clk.rate()), 1.0f, 1e-9f);
        CHECK_INT_EQ(static_cast<int>(clk.setRate(4.0)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(clk.setRate(0.25)),
                     static_cast<int>(VideoResult::Ok));
    }

    TEST_CASE(SyncClockAudioMasterGatedUntilV2) {
        // design.md §9.2: AudioMaster is a V2 feature; the engine-clock
        // fallback applies until then.
        AYVideoSyncClock clk(&FakeNow::tick);
        CHECK_INT_EQ(static_cast<int>(clk.setSource(SyncSource::AudioMaster)),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_INT_EQ(static_cast<int>(clk.source()),
                     static_cast<int>(SyncSource::EngineClock));
        CHECK_INT_EQ(static_cast<int>(clk.setSource(SyncSource::EngineClock)),
                     static_cast<int>(VideoResult::Ok));
    }

TEST_SUITE_END
