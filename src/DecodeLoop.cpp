#include "DecodeLoop.h"

#include "AudioQueue.h"
#include "FrameQueue.h"

#include <chrono>
#include <cstdint>
#include <thread>

namespace ayt::video
{

DecodeLoop::DecodeLoop(IAYVideoDemuxer& demuxer, IAYVideoDecoder& decoder,
                       FrameQueue& videoQueue, AudioQueue* audioQueue,
                       DecodeLoopOptions options)
    : _demuxer(demuxer)
    , _decoder(decoder)
    , _videoQueue(videoQueue)
    , _audioQueue(audioQueue)
    , _options(options)
{
}

DecodeLoop::~DecodeLoop()
{
    requestStop();
    join();
}

void DecodeLoop::start()
{
    _cancel.store(false);
    _finished.store(false);
    _endedClean.store(false);
    _failure.store(VideoResult::Ok);
    _skippedErrors.store(0);
    _reconnectAttempts.store(0);
    _thread = std::thread(&DecodeLoop::run, this);
}

void DecodeLoop::requestStop() noexcept
{
    _cancel.store(true);
    _demuxer.requestAbort();
}

void DecodeLoop::join() noexcept
{
    if (_thread.joinable())
    {
        _thread.join();
    }
}

void DecodeLoop::run() noexcept
{
    _running.store(true);

    VideoPacket pkt;
    VideoFrame frame;
    AudioPcmFrame audio;
    uint32_t reconnectUsed = 0;

    // Drain decode output into the queues until Ok+null / EOS / error /
    // cancel. Returns false when the outer loop must stop.
    auto pumpFrames = [&](bool& eosSeen) -> bool {
        for (;;)
        {
            if (_cancel.load())
            {
                return false;
            }
            const VideoResult fr = _decoder.dequeueFrame(frame);
            if (fr == VideoResult::Ok && frame.data)
            {
                _videoQueue.push(frame); // blocking backpressure (§6.3)
                continue;
            }
            if (fr == VideoResult::EndOfStream)
            {
                eosSeen = true;
                return false;
            }
            if (fr == VideoResult::DecodeError)
            {
                // V4 soft-skip: drop a bad frame and keep draining.
                _skippedErrors.fetch_add(1);
                continue;
            }
            if (fr != VideoResult::Ok)
            {
                _failure.store(fr);
                return false;
            }
            // Ok + null video: drain any pending audio, then yield.
            if (_audioQueue)
            {
                for (;;)
                {
                    if (_cancel.load())
                    {
                        return false;
                    }
                    const VideoResult ar = _decoder.dequeueAudioFrame(audio);
                    if (ar == VideoResult::Ok && audio.data && audio.frameCount > 0)
                    {
                        _audioQueue->push(audio);
                        continue;
                    }
                    if (ar == VideoResult::EndOfStream)
                    {
                        // Audio EOS alone does not end the loop — video
                        // may still be draining. Treat as "no more audio".
                        break;
                    }
                    if (ar != VideoResult::Ok)
                    {
                        _failure.store(ar);
                        return false;
                    }
                    break; // Ok + null
                }
            }
            return true;
        }
    };

    auto feedOne = [&](const VideoPacket& packet) -> bool {
        for (;;)
        {
            if (_cancel.load())
            {
                return false;
            }
            const VideoResult fr = _decoder.feedPacket(packet);
            if (fr == VideoResult::QueueFull)
            {
                bool eosSeen = false;
                if (!pumpFrames(eosSeen))
                {
                    if (eosSeen)
                    {
                        _endedClean.store(true);
                    }
                    return false;
                }
                continue;
            }
            if (fr == VideoResult::DecodeError)
            {
                // V4 soft-skip: drop this packet and continue demuxing.
                _skippedErrors.fetch_add(1);
                return true;
            }
            if (fr != VideoResult::Ok)
            {
                _failure.store(fr);
                return false;
            }
            bool eosSeen = false;
            if (!pumpFrames(eosSeen))
            {
                if (eosSeen)
                {
                    _endedClean.store(true);
                }
                return false;
            }
            return true;
        }
    };

    for (;;)
    {
        if (_cancel.load())
        {
            break;
        }

        const VideoResult r = _demuxer.readNextPacket(pkt);
        if (r == VideoResult::Ok)
        {
            reconnectUsed = 0; // healthy read resets the streak
            if (!feedOne(pkt))
            {
                break;
            }
            continue;
        }

        if (r == VideoResult::EndOfStream)
        {
            VideoPacket end{};
            end.isVideo = true;
            (void)_decoder.feedPacket(end);
            if (_audioQueue)
            {
                VideoPacket aend{};
                aend.isVideo = false;
                (void)_decoder.feedPacket(aend);
            }
            bool eosSeen = false;
            if (!pumpFrames(eosSeen) && eosSeen)
            {
                _endedClean.store(true);
                break;
            }
            if (_decoder.flush() != VideoResult::Ok)
            {
                break;
            }
            eosSeen = false;
            pumpFrames(eosSeen);
            _endedClean.store(true);
            break;
        }

        if (r == VideoResult::DemuxError)
        {
            if (_options.reconnectMax > 0)
            {
                if (reconnectUsed >= _options.reconnectMax)
                {
                    _failure.store(VideoResult::DemuxError);
                    break;
                }
                const uint32_t delayMs =
                    _options.reconnectDelayMs > 0 ? _options.reconnectDelayMs
                                                  : 1u;
                // Short sleep so tests stay fast; cancel can abort early.
                for (uint32_t slept = 0; slept < delayMs; slept += 5)
                {
                    if (_cancel.load())
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (_cancel.load())
                {
                    break;
                }
                ++reconnectUsed;
                _reconnectAttempts.fetch_add(1);
                if (_demuxer.reconnect() == VideoResult::Ok)
                {
                    (void)_decoder.flush();
                    continue;
                }
                // Failed reopen — count against max and retry / fail.
                continue;
            }
            // V4 soft-skip: mid-stream demux glitch — continue reading.
            _skippedErrors.fetch_add(1);
            continue;
        }

        _failure.store(r);
        break;
    }

    _running.store(false);
    _finished.store(true);
}

} // namespace ayt::video
