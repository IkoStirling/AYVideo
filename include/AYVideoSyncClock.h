#pragma once
// AYVideoSyncClock.h — A/V synchronization clock.
//
// design.md §9: master/slave presentation clock. V0.5 shipped the
// engine-clock path; V2 enables AudioMaster when a position provider is
// installed (AYAudio voicePositionFrames via the PCM bridge, §11).

#include <AYVideoTypes.h>
#include <AYTime/Duration.h>
#include <AYTime/TimePoint.h>

#include <cstdint>

namespace ayt::video
{

// Wall-clock provider used for the EngineClock source. Tests inject a
// fake to keep position() deterministic (design.md §19); nullptr uses
// ayt::time::TimePoint::now().
using NowFn = ayt::time::TimePoint (*)() noexcept;

// Audio-master position provider (design.md §9.2 / §11). Returns the
// current media time driven by the audio render head. nullptr = not
// installed → setSource(AudioMaster) returns InvalidState.
using AudioMasterFn = ayt::time::Duration (*)(void* user) noexcept;

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

    // Anchors the engine-clock timeline so position() == mediaStart at
    // the call. AudioMaster ignores the wall anchor but still records
    // mediaStart for pause bookkeeping after a source switch.
    void reset(const ayt::time::Duration& mediaStart = {}) noexcept;

    // Install / clear the audio-master position provider. Must be set
    // before setSource(AudioMaster) succeeds.
    void setAudioMasterProvider(AudioMasterFn fn, void* user = nullptr) noexcept;

    // Sets the master source. AudioMaster requires a provider (else
    // InvalidState; source unchanged).
    VideoResult setSource(SyncSource source) noexcept;
    SyncSource source() const noexcept;

    // Current presentation position. When no anchor is set yet (engine
    // path) or no provider (audio path before install), returns 0.
    ayt::time::Duration position() const noexcept;

    // Playback rate consumed by the EngineClock integral. Valid range
    // [0.25, 4.0]; out-of-range returns InvalidArgument and leaves the
    // rate unchanged (design.md §9.3 — reject, never silently clamp).
    // AudioMaster position is the audio render head (rate does not
    // scale it — audio skip/resample is outside V2).
    VideoResult setRate(double rate) noexcept;
    double rate() const noexcept;

    // Pause bookkeeping — call at the player's Paused/Playing edges so
    // position() is stable while paused (design.md §9.4).
    void markPaused() noexcept;
    void markResumed() noexcept;

    // Drift window for slave (video) vs master (design.md §9.2). Default
    // ±40 ms. Used by pullFrame presentation gating.
    void setDriftTolerance(const ayt::time::Duration& tol) noexcept;
    ayt::time::Duration driftTolerance() const noexcept;

private:
    ayt::time::TimePoint wallNow() const noexcept;
    ayt::time::Duration computePosition() const noexcept;

    NowFn _now = nullptr;
    AudioMasterFn _audioMaster = nullptr;
    void* _audioMasterUser = nullptr;
    SyncSource _source = SyncSource::EngineClock;
    double _rate = 1.0;
    bool _paused = false;
    bool _anchored = false;                    // true after reset();
                                               // position()==0 until then
                                               // (engine path)
    ayt::time::Duration _anchorMediaStart{};   // media time at position 0
    ayt::time::TimePoint _anchorWall{};        // wall clock at last
                                               // resume/reset (engine path)
    ayt::time::Duration _pausedAt{};           // position frozen at
                                               // markPaused()
    ayt::time::Duration _driftTol =
        ayt::time::Duration::fromMs(40);       // §9.2 default ±40 ms
};

} // namespace ayt::video
