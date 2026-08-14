#include "DecodeLoop.h"

#include "FrameQueue.h"

#include <cstdint>

namespace ayt::video
{

DecodeLoop::DecodeLoop(IAYVideoDemuxer& demuxer, IAYVideoDecoder& decoder,
                       FrameQueue& queue)
    : _demuxer(demuxer)
    , _decoder(decoder)
    , _queue(queue)
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
    _thread = std::thread(&DecodeLoop::run, this);
}

void DecodeLoop::requestStop() noexcept
{
    _cancel.store(true);
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

    // Drain decode output into the queue until Ok+null / EOS / error /
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
                _queue.push(frame); // blocking backpressure (§6.3)
                continue;
            }
            if (fr == VideoResult::EndOfStream)
            {
                eosSeen = true;
                return false;
            }
            if (fr != VideoResult::Ok)
            {
                _failure.store(fr);
                return false;
            }
            // Ok + null: no frame ready yet (design.md §6.2).
            return true;
        }
    };

    // Feed one demuxed packet, retrying on QueueFull (EAGAIN) after a
    // decode drain. Returns false when the outer loop must stop.
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
                continue; // retry the same packet
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
            if (!feedOne(pkt))
            {
                break;
            }
            continue;
        }

        if (r == VideoResult::EndOfStream)
        {
            // §8.3 EOS drain: null packet enters FFmpeg drain mode;
            // Mock backends ignore payload and need flush() for EOS.
            VideoPacket end{};
            end.isVideo = true;
            (void)_decoder.feedPacket(end);
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

        // Demux error mid-stream (e.g. container corruption).
        _failure.store(r);
        break;
    }

    _running.store(false);
    _finished.store(true);
}

} // namespace ayt::video
