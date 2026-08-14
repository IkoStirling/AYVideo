#pragma once
// AYVideoPlayer.h — playback control facade.
//
// design.md §10: state machine + control surface. V0.5 shipped the state
// machine skeleton with pluggable Null/Mock backends; V1 wires the real
// pipeline (design.md §8.3): play() starts a dedicated decode thread
// (std::thread, amendment A-03) feeding an SPSC frame queue (§6.3), and
// pullFrame() presents frames when the sync clock reaches their pts.
//
// Thread contract (design.md §14): AYVideoPlayer is a main-thread
// object. Its public surface must only be called from one thread at a
// time; the internal decode thread only touches the backends and the
// frame queue. Events fire synchronously on the calling thread and must
// not re-enter the control surface (design.md §10.4, A-12).

#include <AYVideoMediaInfo.h>
#include <AYVideoSyncClock.h>
#include <AYVideoTypes.h>
#include <IAYVideoBackendFactory.h>
#include <IAYVideoDecoder.h>
#include <IAYVideoDemuxer.h>
#include <aytime/Duration.h>

#include <functional>
#include <memory>
#include <string>

namespace ayt::video
{

class DecodeLoop;
class FrameQueue;
struct QueuedFrame;

// ---------------------------------------------------------------------------
// Player lifecycle states (design.md §10.1; V1 amendments A-06: EOS lands
// in Ready, decode errors land in Failed — see §20).
// ---------------------------------------------------------------------------
enum class PlayerState : uint8_t
{
    Idle,      // constructed, no media open
    Opening,   // open() in flight (V1: synchronous; dwell state)
    Ready,     // media open, stopped at position 0 (or at end after EOS)
    Playing,   // presentation clock running
    Paused,    // presentation clock stopped
    Seeking,   // seek in flight (V1: synchronous internal dwell)
    Stopped,   // stop() called; media unloaded
    Failed,    // unrecoverable error; query via lastResult()
    Count
};

const char* toString(PlayerState state) noexcept;

class AYVideoPlayer
{
public:
    // Backends are injected (default = Null pair). `now` is the sync
    // clock's wall-clock provider (tests inject a fake; nullptr =
    // production ayt::time::TimePoint::now()).
    explicit AYVideoPlayer(
        std::unique_ptr<IAYVideoDemuxer> demuxer = makeNullDemuxer(),
        std::unique_ptr<IAYVideoDecoder> decoder = makeNullDecoder(),
        NowFn now = nullptr);

    // Non-copyable; movable (the source's decode pipeline is stopped
    // before members transfer — the loop references its backends).
    AYVideoPlayer(const AYVideoPlayer&) = delete;
    AYVideoPlayer& operator=(const AYVideoPlayer&) = delete;
    AYVideoPlayer(AYVideoPlayer&&) noexcept;
    AYVideoPlayer& operator=(AYVideoPlayer&&) noexcept;
    ~AYVideoPlayer();

    // -- Control surface ---------------------------------------------------

    // Opens a media source. Valid from Idle/Stopped only (InvalidState
    // otherwise). V1: synchronous — Opening is a dwell state. On success
    // state -> Ready, media info available via getMediaInfo.
    VideoResult open(const std::string& path);

    // Starts / resumes presentation. From Ready (fresh open or EOS):
    // replays from 0. From Paused: resumes the paused position.
    VideoResult play();

    // Stops the presentation clock. Valid from Playing/Ready/Paused/
    // Seeking. The decode thread keeps running under backpressure (the
    // queue fills and blocks it, §6.3) — presentation simply stops.
    VideoResult pause();

    // Unloads the media. Valid from Ready/Playing/Paused/Seeking/Failed.
    // Stops and joins the decode thread, closes both backends, state ->
    // Stopped.
    VideoResult stop();

    // Absolute seek. V1: synchronous dwell with the §8.3 flush sequence
    // (join decode thread -> clear queue -> decoder flush -> demuxer
    // seek -> restart when previously playing). Keyframe-level in V1
    // (frame-exact positioning lands in V4, design.md §7.3).
    VideoResult seek(const ayt::time::Duration& target);

    void setLoop(bool loop) noexcept;
    bool isLooping() const noexcept;

    // Playback rate (0.25 .. 4.0, default 1.0); forwarded to the sync
    // clock (design.md §9.3).
    VideoResult setRate(double rate) noexcept;
    double rate() const noexcept;

    // -- Presentation (V1) -------------------------------------------------

    // Pulls the next frame due for presentation (design.md §10.4 + §20
    // A-07). Valid from Playing only (InvalidState otherwise).
    //   Ok + frame        — frame due (frame.pts <= clock position);
    //                       data valid until the next pullFrame/seek/stop
    //   Ok + data == null — no frame due yet (decode behind / next frame
    //                       in the future) — §6.2 semantics
    //   EndOfStream       — the stream ended (fires onEndOfStream once;
    //                       state -> Ready; loop mode restarts silently)
    //   other             — decode failure; state -> Failed
    VideoResult pullFrame(VideoFrame& out);

    // -- Events (V1, design.md §10.4) -------------------------------------
    // Fired synchronously on the calling thread. Callbacks must not
    // re-enter the control surface (A-12) and must not throw.

    void setOnStateChanged(std::function<void(PlayerState)> cb) noexcept;
    void setOnEndOfStream(std::function<void()> cb) noexcept;

    // -- Queries -----------------------------------------------------------

    PlayerState state() const noexcept;
    bool isPlaying() const noexcept;

    // Backend seams (design.md §7/§8): raw access for pipeline
    // diagnostics and tests. Ownership stays with the player. NOTE: while
    // the decode thread runs (state Playing/Paused) the backends are
    // owned by the loop — read seams only from Ready/Stopped or after
    // stop()/seek() joined the loop.
    IAYVideoDemuxer* demuxer() const noexcept { return _demuxer.get(); }
    IAYVideoDecoder* decoder() const noexcept { return _decoder.get(); }

    // Media metadata; Ok when state >= Ready, InvalidState otherwise.
    VideoResult getMediaInfo(MediaInfo& outInfo) const;

    // The result of the last failed operation (diagnostic; Failed state).
    VideoResult lastResult() const noexcept;

private:
    // State transition table helper (design.md §10.4): returns Ok and
    // applies `to` when `from -> to` is legal, InvalidState otherwise.
    // Fires onStateChanged for applied transitions.
    VideoResult transition(PlayerState from, PlayerState to) noexcept;

    void notifyStateChanged(PlayerState state) noexcept;
    void notifyEndOfStream() noexcept;

    void startLoop();                          // spawn decode thread
    void teardownPipeline() noexcept;          // stop+join+clear (stop/seek/
                                               // move/dtor path, §8.3)

    PlayerState _state = PlayerState::Idle;
    VideoResult _lastResult = VideoResult::Ok;
    bool _loopEnabled = false;       // loop mode (setLoop)
    double _rate = 1.0;

    std::function<void(PlayerState)> _onStateChanged;
    std::function<void()> _onEndOfStream;

    // Pipeline (declared after the backends so they are destroyed first:
    // the loop is joined before the backends go away — see dtor).
    std::unique_ptr<IAYVideoDemuxer> _demuxer;
    std::unique_ptr<IAYVideoDecoder> _decoder;
    MediaInfo _info{};
    std::string _path;

    // Declared AFTER the backends so they are destroyed FIRST (reverse
    // declaration order) — the loop is joined before the backends die.
    // Incomplete types are fine here: the destructor is defined in the
    // .cpp (which includes src/FrameQueue.h + src/DecodeLoop.h).
    std::unique_ptr<FrameQueue> _queue;        // SPSC frame ring (§6.3)
    std::unique_ptr<DecodeLoop> _loop;         // null while not playing
    std::unique_ptr<QueuedFrame> _held;        // clock-gated presentation
                                               // hold (A-07); null = none
    std::unique_ptr<QueuedFrame> _presented;   // last delivered frame's
                                               // pixel storage (§4.5)
    AYVideoSyncClock _clock;
};

} // namespace ayt::video
