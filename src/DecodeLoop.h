#pragma once
// DecodeLoop.h — dedicated decode thread (design.md §8.3).
//
// Owns the demux→decode→queue pipeline on one std::thread (deviation
// A-03: AYTask is a short-task job pool with no persistent-thread
// semantics, so V1 uses std::thread — see design.md §20).
//
// Loop body: readNextPacket → feedPacket → dequeueFrame → push into the
// SPSC FrameQueue (blocking backpressure). On demux EndOfStream: flush
// the decoder, drain remaining frames, mark clean end. On cancel
// (stop/seek): exit at the next safe point. Backends are referenced,
// not owned — the player owns them and must join the loop before
// closing them (§8.3 flush sequence).
//
// Thread contract (design.md §4.4/§14): the decode thread is the ONLY
// thread that touches the demuxer/decoder while the loop runs.

#include <AYVideoTypes.h>
#include <IAYVideoDecoder.h>
#include <IAYVideoDemuxer.h>

#include <atomic>
#include <thread>

namespace ayt::video
{

class FrameQueue;

class DecodeLoop
{
public:
    DecodeLoop(IAYVideoDemuxer& demuxer, IAYVideoDecoder& decoder,
               FrameQueue& queue);
    ~DecodeLoop();

    DecodeLoop(const DecodeLoop&) = delete;
    DecodeLoop& operator=(const DecodeLoop&) = delete;

    void start();
    void requestStop() noexcept;
    void join() noexcept;

    bool running() const noexcept { return _running.load(); }
    // True after the loop reached demux EOS + decoder drained.
    bool endedCleanly() const noexcept { return _endedClean.load(); }
    // The failure that stopped the loop (Ok = none; e.g. DecodeError).
    VideoResult failure() const noexcept { return _failure.load(); }
    // True when the loop exited (cleanly or not).
    bool finished() const noexcept { return _finished.load(); }

private:
    void run() noexcept;

    IAYVideoDemuxer& _demuxer;
    IAYVideoDecoder& _decoder;
    FrameQueue& _queue;

    std::thread _thread;
    std::atomic<bool> _cancel{false};
    std::atomic<bool> _running{false};
    std::atomic<bool> _finished{false};
    std::atomic<bool> _endedClean{false};
    std::atomic<VideoResult> _failure{VideoResult::Ok};
};

} // namespace ayt::video
