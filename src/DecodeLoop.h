#pragma once
// DecodeLoop.h — dedicated decode thread (design.md §8.3).
//
// Owns the demux→decode→queue pipeline on one std::thread (deviation
// A-03: AYTask is a short-task job pool with no persistent-thread
// semantics, so V1 uses std::thread — see design.md §20).
//
// Loop body: readNextPacket → feedPacket → dequeueFrame[/Audio] → push
// into the SPSC FrameQueue (blocking) and optional AudioQueue (drop-
// oldest on overflow, §11). On demux EndOfStream: flush the decoder,
// drain remaining frames, mark clean end.
//
// In-loop seek: the player posts requestSeek(); this thread alone
// touches demuxer.seek + decoder.flush (A-07). The player clears the
// SPSC queues (consumer side) around the request.
//
// PlaybackIntent (player sole writer): ScrubPreview / CatchUpToFloor /
// Playing — decode only reads it so scrub ceiling cannot race play().

#include <AYVideo/VideoTypes.h>
#include <AYVideo/IVideoDecoder.h>
#include <AYVideo/IVideoDemuxer.h>
#include <AYTime/Duration.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace ayt::video
{

class FrameQueue;
class AudioQueue;
class SubtitleCueQueue;

// In-loop seek kind (mirrors AYVideoPlayer::SeekMode).
enum class InLoopSeekMode : uint8_t
{
    Accurate = 0, // demux KF, drop enqueue until pts >= target
    Keyframe = 1, // demux KF, enqueue first frame only (preview)
    Scrub = 2,    // demux KF, enqueue frames up to a live ceiling (WMP-like)
};

// Player-owned decode behaviour. Decode thread only loads this.
enum class PlaybackIntent : uint8_t
{
    Playing = 0,       // steady playback
    ScrubPreview = 1,  // drag: enqueue pts <= ceiling; pause past ceiling
    CatchUpToFloor = 2 // play-after-scrub / Accurate: drop pts < floor
};

struct DecodeLoopOptions
{
    // V5: when > 0, DemuxError triggers demuxer.reconnect() + decoder
    // flush up to this many times before failing the loop. 0 = V4
    // soft-skip only (local files).
    uint32_t reconnectMax = 0;
    uint32_t reconnectDelayMs = 500;
};

class DecodeLoop
{
public:
    // `audioQueue` / `subtitleCues` may be null. When subtitleCues is set,
    // subtitle packets are diverted into that mailbox (never fed to the
    // A/V decoder).
    DecodeLoop(IAYVideoDemuxer& demuxer, IAYVideoDecoder& decoder,
               FrameQueue& videoQueue, AudioQueue* audioQueue = nullptr,
               DecodeLoopOptions options = {},
               SubtitleCueQueue* subtitleCues = nullptr);
    ~DecodeLoop();

    DecodeLoop(const DecodeLoop&) = delete;
    DecodeLoop& operator=(const DecodeLoop&) = delete;

    void start();
    void requestStop() noexcept;
    void join() noexcept;

    // Post an in-loop seek. Coalesces: only the latest target is applied.
    // Returns the serial the caller should wait on via seekApplied().
    // Safe from the player thread while the loop is running.
    // `videoStreamIndex` / `audioStreamIndex` of -2 mean "leave active
    // streams unchanged"; other values are applied via
    // setActiveStreamIndices on the decode thread before demux seek.
    uint64_t requestSeek(const ayt::time::Duration& target, InLoopSeekMode mode,
                         int32_t videoStreamIndex = -2,
                         int32_t audioStreamIndex = -2) noexcept;

    // WMP-style scrub: raise/lower the display ceiling without a demux
    // re-seek when still inside the current GOP walk. Returns true when
    // the ceiling was updated in place (no new serial).
    bool updateScrubTarget(const ayt::time::Duration& target) noexcept;

    void endScrubMode() noexcept;

    // Player sole writer — publish intent before mutating queues / play().
    void setIntent(PlaybackIntent intent) noexcept
    {
        _intent.store(static_cast<uint8_t>(intent), std::memory_order_release);
        if (intent != PlaybackIntent::ScrubPreview)
        {
            endScrubMode();
        }
    }

    PlaybackIntent intent() const noexcept
    {
        return static_cast<PlaybackIntent>(
            _intent.load(std::memory_order_acquire));
    }

    bool scrubActive() const noexcept
    {
        return _scrubActive.load(std::memory_order_acquire);
    }

    bool scrubPausedAtCeiling() const noexcept
    {
        return _scrubPausedAtCeiling.load(std::memory_order_acquire);
    }

    uint64_t seekSerial() const noexcept { return _seekSerial.load(); }
    uint64_t seekAppliedSerial() const noexcept
    {
        return _seekAppliedSerial.load();
    }
    VideoResult lastSeekResult() const noexcept { return _seekResult.load(); }

    // Pts floor for post-seek enqueue (0 = none; INT64_MAX = waiting for
    // first post-seek packet). Player harvest should ignore older frames.
    std::int64_t postSeekMinPtsUs() const noexcept
    {
        return _postSeekMinPtsUs.load(std::memory_order_acquire);
    }

    std::int64_t scrubLastOutPtsUs() const noexcept
    {
        return _scrubLastOutPtsUs.load(std::memory_order_acquire);
    }

    // Arm Accurate-style catch-up without a demux re-seek.
    void armDropBelow(const ayt::time::Duration& floor) noexcept
    {
        const std::int64_t us = floor.toUs();
        _dropBelowPtsUs.store(us > 0 ? us : 0, std::memory_order_release);
    }

    // Spin-wait until seekAppliedSerial >= serial (or cancel/finish).
    bool waitSeekApplied(uint64_t serial, uint32_t timeoutMs) const noexcept;

    bool running() const noexcept { return _running.load(); }
    bool endedCleanly() const noexcept { return _endedClean.load(); }
    VideoResult failure() const noexcept { return _failure.load(); }
    bool finished() const noexcept { return _finished.load(); }
    uint32_t skippedErrors() const noexcept { return _skippedErrors.load(); }
    uint32_t reconnectAttempts() const noexcept
    {
        return _reconnectAttempts.load();
    }

private:
    void run() noexcept;
    bool applyPendingSeek() noexcept;
    void restoreOverflowPolicy() noexcept;
    void armScrubPreview(int64_t targetUs) noexcept;
    void armCatchUpOrPlaying(InLoopSeekMode mode, int64_t targetUs) noexcept;

    IAYVideoDemuxer& _demuxer;
    IAYVideoDecoder& _decoder;
    FrameQueue& _videoQueue;
    AudioQueue* _audioQueue = nullptr;
    SubtitleCueQueue* _subtitleCues = nullptr;
    DecodeLoopOptions _options{};

    std::thread _thread;
    std::atomic<bool> _cancel{false};
    std::atomic<bool> _running{false};
    std::atomic<bool> _finished{false};
    std::atomic<bool> _endedClean{false};
    std::atomic<VideoResult> _failure{VideoResult::Ok};
    std::atomic<uint32_t> _skippedErrors{0};
    std::atomic<uint32_t> _reconnectAttempts{0};

    // In-loop seek mailbox (player → decode).
    std::atomic<uint64_t> _seekSerial{0};
    std::atomic<uint64_t> _seekAppliedSerial{0};
    std::atomic<int64_t> _seekTargetUs{0};
    std::atomic<uint8_t> _seekMode{static_cast<uint8_t>(InLoopSeekMode::Accurate)};
    std::atomic<int32_t> _seekVideoStream{-2};
    std::atomic<int32_t> _seekAudioStream{-2};
    std::atomic<VideoResult> _seekResult{VideoResult::Ok};
    std::atomic<int64_t> _dropBelowPtsUs{0};
    std::atomic<int64_t> _postSeekMinPtsUs{0};

    // Player-owned intent (decode reads only).
    std::atomic<uint8_t> _intent{static_cast<uint8_t>(PlaybackIntent::Playing)};

    std::atomic<bool> _scrubActive{false};
    std::atomic<bool> _scrubPausedAtCeiling{false};
    std::atomic<int64_t> _scrubCeilingUs{0};
    std::atomic<int64_t> _scrubLandPtsUs{0};
    std::atomic<int64_t> _scrubLastOutPtsUs{0};

    std::chrono::steady_clock::time_point _dropBelowArmedAt{};
    int32_t _appliedVideoStream = -2;
    int32_t _appliedAudioStream = -2;
    bool _logNextVideoPts = false;
    uint8_t _savedOverflowPolicy = 0;
    bool _overflowPolicySaved = false;

    // Decode-thread only: first frame past scrub ceiling (not destroyed).
    struct PendingFrame
    {
        VideoFrame frame;
        std::vector<uint8_t> pixels;
        uint64_t seekSerial = 0;
    };
    std::optional<PendingFrame> _pendingOverCeiling;
};

} // namespace ayt::video
