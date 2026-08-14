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
// Thread contract (design.md §4.4/§14): the decode thread is the ONLY
// thread that touches the demuxer/decoder while the loop runs.

#include <AYVideoTypes.h>
#include <IAYVideoDecoder.h>
#include <IAYVideoDemuxer.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace ayt::video
{

class FrameQueue;
class AudioQueue;

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
    // `audioQueue` may be null (video-only). When non-null the decoder
    // must have been opened with decodeAudio=true.
    DecodeLoop(IAYVideoDemuxer& demuxer, IAYVideoDecoder& decoder,
               FrameQueue& videoQueue, AudioQueue* audioQueue = nullptr,
               DecodeLoopOptions options = {});
    ~DecodeLoop();

    DecodeLoop(const DecodeLoop&) = delete;
    DecodeLoop& operator=(const DecodeLoop&) = delete;

    void start();
    void requestStop() noexcept;
    void join() noexcept;

    bool running() const noexcept { return _running.load(); }
    bool endedCleanly() const noexcept { return _endedClean.load(); }
    VideoResult failure() const noexcept { return _failure.load(); }
    bool finished() const noexcept { return _finished.load(); }
    // V4 soft-skip: mid-stream DemuxError/DecodeError packets dropped.
    uint32_t skippedErrors() const noexcept { return _skippedErrors.load(); }
    // V5: successful demuxer.reconnect() calls from this loop.
    uint32_t reconnectAttempts() const noexcept
    {
        return _reconnectAttempts.load();
    }

private:
    void run() noexcept;

    IAYVideoDemuxer& _demuxer;
    IAYVideoDecoder& _decoder;
    FrameQueue& _videoQueue;
    AudioQueue* _audioQueue = nullptr;
    DecodeLoopOptions _options{};

    std::thread _thread;
    std::atomic<bool> _cancel{false};
    std::atomic<bool> _running{false};
    std::atomic<bool> _finished{false};
    std::atomic<bool> _endedClean{false};
    std::atomic<VideoResult> _failure{VideoResult::Ok};
    std::atomic<uint32_t> _skippedErrors{0};
    std::atomic<uint32_t> _reconnectAttempts{0};
};

} // namespace ayt::video
