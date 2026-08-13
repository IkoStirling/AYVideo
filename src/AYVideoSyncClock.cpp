#include <AYVideoSyncClock.h>

namespace ayt::video
{

const char* toString(SyncSource source) noexcept
{
    switch (source)
    {
    case SyncSource::EngineClock: return "EngineClock";
    case SyncSource::AudioMaster: return "AudioMaster";
    case SyncSource::Count:       return "Count";
    }
    return "Unknown";
}

namespace
{

constexpr double kMinRate = 0.25;
constexpr double kMaxRate = 4.0;

} // namespace

AYVideoSyncClock::AYVideoSyncClock(NowFn now) noexcept
    : _now(now)
{
}

ayt::time::TimePoint AYVideoSyncClock::wallNow() const noexcept
{
    return _now ? _now() : ayt::time::TimePoint::now();
}

void AYVideoSyncClock::reset(const ayt::time::Duration& mediaStart) noexcept
{
    _anchorMediaStart = mediaStart;
    _anchorWall = wallNow();
    _paused = false;
    _pausedAt = {};
    _anchored = true;
}

VideoResult AYVideoSyncClock::setSource(SyncSource source) noexcept
{
    if (source == SyncSource::AudioMaster)
    {
        // design.md §9.2: audio-master arrives with the V2 AYAudio
        // bridge; until then the engine-clock fallback applies.
        return VideoResult::InvalidState;
    }
    _source = source;
    return VideoResult::Ok;
}

SyncSource AYVideoSyncClock::source() const noexcept
{
    return _source;
}

ayt::time::Duration AYVideoSyncClock::computePosition() const noexcept
{
    if (!_anchored)
    {
        // No anchor set yet: position is 0 until reset() (design.md §9.1).
        return {};
    }
    if (_paused)
    {
        return _pausedAt;
    }
    const ayt::time::Duration elapsed = wallNow() - _anchorWall;
    // position = mediaStart + elapsed * rate  (engine-clock path)
    const double us = static_cast<double>(elapsed.toUs()) * _rate;
    return _anchorMediaStart + ayt::time::Duration::fromUs(
        static_cast<std::int64_t>(us));
}

ayt::time::Duration AYVideoSyncClock::position() const noexcept
{
    return computePosition();
}

VideoResult AYVideoSyncClock::setRate(double rate) noexcept
{
    if (rate < kMinRate || rate > kMaxRate)
    {
        return VideoResult::InvalidArgument;
    }
    _rate = rate;
    return VideoResult::Ok;
}

double AYVideoSyncClock::rate() const noexcept
{
    return _rate;
}

void AYVideoSyncClock::markPaused() noexcept
{
    if (!_paused)
    {
        _pausedAt = computePosition();
        _paused = true;
    }
}

void AYVideoSyncClock::markResumed() noexcept
{
    if (_paused)
    {
        // Re-anchor the wall clock so position continuity holds across
        // the pause boundary.
        _anchorMediaStart = _pausedAt;
        _anchorWall = wallNow();
        _paused = false;
    }
}

} // namespace ayt::video
