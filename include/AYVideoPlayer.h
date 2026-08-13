#pragma once
// AYVideoPlayer.h — playback control facade.
//
// design.md §10: state machine + control surface. V0.5 ships the state
// machine skeleton with pluggable Null/Mock backends (real decode
// pipeline wiring lands in V1). All mutators return VideoResult —
// invalid state transitions return InvalidState and leave state
// unchanged (design.md §10.4 transition table).
//
// Thread contract (design.md §14): AYVideoPlayer is a main-thread
// object. It may internally own worker threads (V1+) but its own public
// surface must only be called from one thread at a time.

#include <AYVideoMediaInfo.h>
#include <AYVideoSyncClock.h>
#include <AYVideoTypes.h>
#include <IAYVideoBackendFactory.h>
#include <IAYVideoDecoder.h>
#include <IAYVideoDemuxer.h>
#include <aytime/Duration.h>

#include <memory>
#include <string>

namespace ayt::video
{

// ---------------------------------------------------------------------------
// Player lifecycle states (design.md §10.1).
// ---------------------------------------------------------------------------
enum class PlayerState : uint8_t
{
    Idle,      // constructed, no media open
    Opening,   // open() in flight (V1: async open)
    Ready,     // media open, paused at position 0
    Playing,   // presentation clock running
    Paused,    // presentation clock stopped
    Seeking,   // seek in flight (V1: internal; transitions back to
               // Ready/Playing/Paused per §10.4)
    Stopped,   // stop() called; media unloaded
    Failed,    // unrecoverable error; query via lastResult()
    Count
};

const char* toString(PlayerState state) noexcept;

class AYVideoPlayer
{
public:
    // Backends are injected (default = Null pair). V1 adds an overload
    // that constructs the FFmpeg pair via the factory.
    explicit AYVideoPlayer(
        std::unique_ptr<IAYVideoDemuxer> demuxer = makeNullDemuxer(),
        std::unique_ptr<IAYVideoDecoder> decoder = makeNullDecoder());

    // Non-copyable; movable.
    AYVideoPlayer(const AYVideoPlayer&) = delete;
    AYVideoPlayer& operator=(const AYVideoPlayer&) = delete;
    AYVideoPlayer(AYVideoPlayer&&) noexcept;
    AYVideoPlayer& operator=(AYVideoPlayer&&) noexcept;
    ~AYVideoPlayer();

    // -- Control surface ---------------------------------------------------

    // Opens a media source. Valid from Idle/Stopped only (InvalidState
    // otherwise). On success state -> Ready, media info available via
    // getMediaInfo.
    VideoResult open(const std::string& path);

    // Starts / resumes presentation. Valid from Ready/Paused.
    VideoResult play();

    // Stops the presentation clock. Valid from Playing/Ready/Paused/
    // Seeking.
    VideoResult pause();

    // Unloads the media. Valid from Ready/Playing/Paused/Seeking/Failed.
    // State -> Stopped.
    VideoResult stop();

    // Absolute seek. V0.5 skeleton validates the transition (state ->
    // Seeking -> back to pre-seek state) and forwards to the demuxer;
    // precise frame positioning lands in V1/V4 (design.md §7.3).
    VideoResult seek(const ayt::time::Duration& target);

    void setLoop(bool loop) noexcept;
    bool isLooping() const noexcept;

    // Playback rate (0.25 .. 4.0, default 1.0). V0.5 stores; the sync
    // clock consumes it from V1 (design.md §9.3).
    VideoResult setRate(double rate) noexcept;
    double rate() const noexcept;

    // -- Queries -----------------------------------------------------------

    PlayerState state() const noexcept;
    bool isPlaying() const noexcept;

    // Backend seams (design.md §7/§8): raw access for pipeline
    // diagnostics and tests. Ownership stays with the player.
    IAYVideoDemuxer* demuxer() const noexcept { return _demuxer.get(); }
    IAYVideoDecoder* decoder() const noexcept { return _decoder.get(); }

    // Media metadata; Ok when state >= Ready, InvalidState otherwise.
    VideoResult getMediaInfo(MediaInfo& outInfo) const;

    // The result of the last failed operation (diagnostic; Failed state).
    VideoResult lastResult() const noexcept;

    // V1: event registration (onStateChanged / onEndOfStream) —
    // design.md §10.5.

private:
    // State transition table helper (design.md §10.4): returns Ok and
    // applies `to` when `from -> to` is legal, InvalidState otherwise.
    VideoResult transition(PlayerState from, PlayerState to) noexcept;

    PlayerState _state = PlayerState::Idle;
    VideoResult _lastResult = VideoResult::Ok;
    bool _loop = false;
    double _rate = 1.0;

    std::unique_ptr<IAYVideoDemuxer> _demuxer;
    std::unique_ptr<IAYVideoDecoder> _decoder;
    MediaInfo _info{};
    std::string _path;
};

} // namespace ayt::video
