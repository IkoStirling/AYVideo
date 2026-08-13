#pragma once
// AYVideoSyncClock.h — A/V synchronization clock.
//
// design.md §9: master/slave presentation clock. V0.5 ships the contract
// + engine-clock fallback stub; the audio-master implementation lands in
// V2 (AudioEngine::streamPush position query via the AYAudio bridge,
// design.md §11). V1 uses the EngineClock source so video-only playback
// runs on ayt::time::Clock::gameNow().
//
// Thread contract (design.md §14): single-threaded (player thread);
// workers read position through atomics in V1+.

#include <AYVideoTypes.h>
#include <aytime/Duration.h>
#include <aytime/TimePoint.h>

#include <cstdint>

namespace ayt::video
{

// Wall-clock provider used for the EngineClock source. Tests inject a
// fake to keep position() deterministic (design.md §19); nullptr uses
// ayt::time::TimePoint::now().
using NowFn = ayt::time::TimePoint (*)() noexcept;

// Master source driving the presentation clock (design.md §9.2).
enum class SyncSource : uint8_t
{
    EngineClock = 0,   // ayt::time::Clock::gameNow()-driven fallback
    AudioMaster,       // V2: audio render position is the master
    Count
};

const char* toString(SyncSource source) noexcept;

class AYVideoSyncClock
{
public:
    explicit AYVideoSyncClock(NowFn now = nullptr) noexcept;

    // V0.5: state + contract. `mediaStart` anchors the timeline so
    // position() == 0 at open (design.md §9.1).
    void reset(const ayt::time::Duration& mediaStart = {}) noexcept;

    // Sets the master source. AudioMaster returns InvalidState until V2
    // (engine clock fallback applies).
    VideoResult setSource(SyncSource source) noexcept;
    SyncSource source() const noexcept;

    // Current presentation position. When no anchor is set yet, returns
    // Duration{} (position 0).
    ayt::time::Duration position() const noexcept;

    // Playback rate consumed by the position integral. Valid range
    // [0.25, 4.0]; out-of-range returns InvalidArgument and leaves the
    // rate unchanged (design.md §9.3 — reject, never silently clamp).
    VideoResult setRate(double rate) noexcept;
    double rate() const noexcept;

    // Pause bookkeeping — call at the player's Paused/Playing edges so
    // position() is stable while paused (design.md §9.4).
    void markPaused() noexcept;
    void markResumed() noexcept;

private:
    ayt::time::TimePoint wallNow() const noexcept;
    ayt::time::Duration computePosition() const noexcept;

    NowFn _now = nullptr;
    SyncSource _source = SyncSource::EngineClock;
    double _rate = 1.0;
    bool _paused = false;
    bool _anchored = false;                    // true after reset();
                                               // position()==0 until then
    ayt::time::Duration _anchorMediaStart{};   // media time at position 0
    ayt::time::TimePoint _anchorWall{};        // wall clock at last
                                               // resume/reset (engine path)
    ayt::time::Duration _pausedAt{};           // position frozen at
                                               // markPaused()
};

} // namespace ayt::video
