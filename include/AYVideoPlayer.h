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

#include <AYVideoAudioFrame.h>
#include <AYVideoMediaInfo.h>
#include <AYVideoSubtitle.h>
#include <AYVideoSyncClock.h>
#include <AYVideoTrack.h>
#include <AYVideoTypes.h>
#include <IAYVideoBackendFactory.h>
#include <IAYVideoDecoder.h>
#include <IAYVideoDemuxer.h>
#include <aytime/Duration.h>

#include <functional>
#include <memory>
#include <string>
#include <chrono>

namespace ayt::audio
{
class AudioEngine;
}

namespace ayt::video
{

class DecodeLoop;
class FrameQueue;
class AudioQueue;
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

    // Absolute seek. Default Accurate (frame-exact commit).
    //
    // Industrial / Windows Media Player-class scrub contract:
    //   * SeekMode::Scrub — interactive drag/click: demux to prior I-frame,
    //     then decode forward enqueueing frames up to a live ceiling that
    //     tracks the thumb. UI shows the latest frame (frame-by-frame while
    //     dragging). Never blocks the UI thread. Prefer this for scrubbers.
    //   * SeekMode::Keyframe — I-frame snapshot only (fastest, picture may
    //     lag the thumb on long GOP). Legacy/preview helper.
    //   * SeekMode::Accurate — silent keyframe→target catch-up; first
    //     presented frame within ±1 CFR frame of `target` (design.md §7.3).
    //     Use for API/frame-exact seeks, not for live scrub drag.
    enum class SeekMode : uint8_t
    {
        Accurate = 0,
        Keyframe = 1,
        Scrub = 2,
    };
    VideoResult seek(const ayt::time::Duration& target,
                     SeekMode mode = SeekMode::Accurate);

    // Drain frames from an in-loop seek into `_held`. Non-blocking.
    // Scrub: keeps the latest decoded frame (frame-by-frame drag).
    // Accurate/Keyframe: first acceptable frame for the mode.
    void pollSeekPreview() noexcept;

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

    // Last held/presented picture without advancing the clock. Valid from
    // Ready/Paused/Playing (scrub preview after SeekMode::Keyframe).
    // Ok + null when no picture is staged yet.
    VideoResult currentFrame(VideoFrame& out) noexcept;

    // -- Events (V1, design.md §10.4) -------------------------------------
    // Fired synchronously on the calling thread. Callbacks must not
    // re-enter the control surface (A-12) and must not throw.

    void setOnStateChanged(std::function<void(PlayerState)> cb) noexcept;
    void setOnEndOfStream(std::function<void()> cb) noexcept;

    // V5: buffering watermark event (stays Playing; no Buffering state).
    // Fired synchronously on the calling thread of pullFrame/play.
    void setOnBufferingChanged(std::function<void(bool)> cb) noexcept;
    bool isBuffering() const noexcept;

    // V5: FrameQueue watermarks for network progressive streams.
    // Enter buffering when queued frames <= low; exit when >= high.
    // Ignored for local files (reconnectMax == 0).
    void setBufferWatermarks(uint32_t low, uint32_t high) noexcept;

    // V2: optional AYAudio engine for PCM bridge + AudioMaster sync
    // (design.md §11). nullptr clears the bridge (EngineClock fallback).
    // Must be called while Idle/Stopped (or before open); InvalidState
    // otherwise. The player does not own the engine.
    VideoResult attachAudioEngine(ayt::audio::AudioEngine* engine) noexcept;

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

    // V4 soft-subtitle track discovery (N-08). Valid from Ready/Playing/
    // Paused/Seeking. setActiveSubtitleTrack stores selection only —
    // cue demux/render is not implemented in this slice (-1 = off).
    uint32_t subtitleTrackCount() const noexcept;
    VideoResult getSubtitleTrack(uint32_t index, SubtitleTrackInfo& out) const;
    VideoResult setActiveSubtitleTrack(int32_t index) noexcept;
    int32_t activeSubtitleTrack() const noexcept;

    // V4 N-10: A/V track discovery + deferred selection (applied on next
    // play/seek via demuxer setActiveStreamIndices). Seamless mid-play
    // hot-swap without flush is out of scope.
    uint32_t videoTrackCount() const noexcept;
    uint32_t audioTrackCount() const noexcept;
    VideoResult getVideoTrack(uint32_t index, VideoTrackInfo& out) const;
    VideoResult getAudioTrack(uint32_t index, AudioTrackInfo& out) const;
    VideoResult setActiveVideoTrack(int32_t index) noexcept;
    VideoResult setActiveAudioTrack(int32_t index) noexcept;
    int32_t activeVideoTrack() const noexcept;
    int32_t activeAudioTrack() const noexcept;

    // Presentation clock position (EngineClock or AudioMaster).
    ayt::time::Duration position() const noexcept;
    SyncSource syncSource() const noexcept;

    // The result of the last failed operation (diagnostic; Failed state).
    VideoResult lastResult() const noexcept;

    // V4 memory-pressure diagnostics (design.md §6.3 / §16).
    struct QueueStats
    {
        uint32_t videoSize = 0;
        uint32_t videoCapacity = 0;
        uint64_t videoDropped = 0;
        uint32_t audioSize = 0;
        uint32_t audioCapacity = 0;
        uint64_t audioDropped = 0;
    };
    QueueStats queueStats() const noexcept;

    // Preview / stress: switch the video FrameQueue overflow policy.
    // Default remains Block. Safe while Idle/Ready/Stopped (or after
    // pause with decode loop torn down); IgnoredInvalid while Playing.
    VideoResult setFrameQueueOverflowPolicy(FrameQueueOverflowPolicy policy) noexcept;

private:
    // State transition table helper (design.md §10.4): returns Ok and
    // applies `to` when `from -> to` is legal, InvalidState otherwise.
    // Fires onStateChanged for applied transitions.
    VideoResult transition(PlayerState from, PlayerState to) noexcept;

    void notifyStateChanged(PlayerState state) noexcept;
    void notifyEndOfStream() noexcept;
    void notifyBufferingChanged(bool buffering) noexcept;
    void updateBufferingFromQueue() noexcept;  // V5 watermarks
    void setBuffering(bool buffering) noexcept;
    void freezeClockAt(const ayt::time::Duration& mediaPos) noexcept;
    void clampClockToDuration() noexcept;
    ayt::time::Duration mediaDuration() const noexcept;
    void armClockGate() noexcept;       // pause until first post-seek frame
    bool tryReleaseClockGate() noexcept;
    // Decode the first video frame on the player thread after a demux
    // seek (decode thread must be stopped). Fills `_held` for instant
    // present and advances demux/decoder to a consistent resume point.
    // When `anyKeyframe` is true, accept the first decoded frame (scrub
    // preview) instead of waiting for pts >= `_minPresentPts`.
    bool primeFirstFrame(bool anyKeyframe = false) noexcept;
    // In-loop seek helpers (decode thread owns demux/decoder).
    bool postInLoopSeek(const ayt::time::Duration& target, SeekMode mode,
                        bool waitApplied, bool waitFirstFrame) noexcept;
    bool harvestSeekFrame(bool anyKeyframe, uint32_t timeoutMs) noexcept;
    // Scrub: drain queue keeping the newest acceptable frame.
    bool harvestScrubLatest() noexcept;

    void startLoop();                          // spawn decode thread
    void teardownPipeline() noexcept;          // stop+join+clear (stop/seek/
                                               // move/dtor path, §8.3)
    void teardownAudioBridge() noexcept;       // close stream / stop voice
    bool ensureAudioBridge() noexcept;         // openStream + playStream
    void pumpAudioToEngine() noexcept;         // AudioQueue → streamPush
    bool presentDueFrame(VideoFrame& out);     // drift-aware presentation
    VideoResult applyActiveTracks() noexcept;  // V4 N-10 demux remap + info refresh
    static ayt::time::Duration audioMasterThunk(void* user) noexcept;

    PlayerState _state = PlayerState::Idle;
    VideoResult _lastResult = VideoResult::Ok;
    bool _loopEnabled = false;       // loop mode (setLoop)
    double _rate = 1.0;

    std::function<void(PlayerState)> _onStateChanged;
    std::function<void()> _onEndOfStream;
    std::function<void(bool)> _onBufferingChanged;

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
    std::unique_ptr<AudioQueue> _audioQueue;   // SPSC PCM ring (§11)
    std::unique_ptr<DecodeLoop> _loop;         // null while not playing
    std::unique_ptr<QueuedFrame> _held;        // clock-gated presentation
                                               // hold (A-07); null = none
    std::unique_ptr<QueuedFrame> _presented;   // last delivered frame's
                                               // pixel storage (§4.5)
    AYVideoSyncClock _clock;
    // V4 seek accuracy: drop decoded frames with pts < this floor
    // (set by seek(); cleared on open/stop / play-from-Ready restart).
    ayt::time::Duration _minPresentPts{};
    // V4: selected subtitle track index into MediaInfo::subtitleTracks,
    // or -1 when off. Selection only — no cue pipeline yet.
    int32_t _activeSubtitleTrack = -1;
    int32_t _activeVideoTrack = 0;   // index into MediaInfo::videoTracks
    int32_t _activeAudioTrack = 0;   // index into MediaInfo::audioTracks (-1 off)
    FrameQueueOverflowPolicy _frameOverflowPolicy =
        FrameQueueOverflowPolicy::Block;

    // V5 network progressive: last open params + buffering watermarks.
    DemuxerOpenParams _demuxParams{};
    bool _networkStreaming = false;
    bool _buffering = false;
    bool _clockGated = false; // true after startLoop until first frame ready
    bool _pipelinePrimed = false; // demux/decoder already at clock after seek
    bool _awaitingSeekPreview = false; // in-loop seek harvest pending
    bool _awaitingSeekKeyframe = false;
    bool _awaitingSeekScrub = false;
    // Accurate async snap: accept any first frame for display but keep the
    // scrub-target floor so play() still drops until pts >= target.
    bool _awaitingSeekPreserveFloor = false;
    bool _seekPreviewNeedsClear = false;
    uint64_t _awaitingSeekSerial = 0;
    // Steady-clock start of Accurate floor wait (for floor.ready logs).
    std::chrono::steady_clock::time_point _floorWaitStarted{};
    uint32_t _bufferLow = 0;   // enter buffering when size <= low
    uint32_t _bufferHigh = 2;  // exit buffering when size >= high

    // AYAudio PCM bridge (not owned). Handles are Invalid* when inactive.
    ayt::audio::AudioEngine* _audioEngine = nullptr;
    uint32_t _audioStreamId = 0;   // ayt::audio::AudioStreamId
    uint32_t _audioVoice = 0;      // ayt::audio::VoiceHandle
};

} // namespace ayt::video
