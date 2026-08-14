#include <AYVideoPlayer.h>

#include "DecodeLoop.h"
#include "FrameQueue.h"

#include <cstdint>

namespace ayt::video
{

const char* toString(PlayerState state) noexcept
{
    switch (state)
    {
    case PlayerState::Idle:    return "Idle";
    case PlayerState::Opening: return "Opening";
    case PlayerState::Ready:   return "Ready";
    case PlayerState::Playing: return "Playing";
    case PlayerState::Paused:  return "Paused";
    case PlayerState::Seeking: return "Seeking";
    case PlayerState::Stopped: return "Stopped";
    case PlayerState::Failed:  return "Failed";
    case PlayerState::Count:   return "Count";
    }
    return "Unknown";
}

namespace
{

// State transition table (design.md §10.4 + §20 A-06: V1 adds
// Playing -> Ready on EOS and Playing -> Failed on decode error).
bool isTransitionLegal(PlayerState from, PlayerState to) noexcept
{
    switch (from)
    {
    case PlayerState::Idle:
        return to == PlayerState::Opening;
    case PlayerState::Opening:
        return to == PlayerState::Ready || to == PlayerState::Failed;
    case PlayerState::Ready:
        return to == PlayerState::Playing || to == PlayerState::Paused
            || to == PlayerState::Seeking || to == PlayerState::Stopped;
    case PlayerState::Playing:
        return to == PlayerState::Paused || to == PlayerState::Seeking
            || to == PlayerState::Stopped || to == PlayerState::Ready // EOS (A-06)
            || to == PlayerState::Failed;                             // decode error (A-06)
    case PlayerState::Paused:
        return to == PlayerState::Playing || to == PlayerState::Seeking
            || to == PlayerState::Stopped;
    case PlayerState::Seeking:
        return to == PlayerState::Ready || to == PlayerState::Playing
            || to == PlayerState::Paused || to == PlayerState::Stopped
            || to == PlayerState::Failed;
    case PlayerState::Stopped:
        return to == PlayerState::Opening; // re-entry via open()
    case PlayerState::Failed:
        return to == PlayerState::Stopped; // recovery via stop()
    case PlayerState::Count:
        return false;
    }
    return false;
}

} // namespace

AYVideoPlayer::AYVideoPlayer(
    std::unique_ptr<IAYVideoDemuxer> demuxer,
    std::unique_ptr<IAYVideoDecoder> decoder,
    NowFn now)
    : _demuxer(std::move(demuxer))
    , _decoder(std::move(decoder))
    , _queue(std::make_unique<FrameQueue>())
    , _clock(now)
{
}

AYVideoPlayer::AYVideoPlayer(AYVideoPlayer&& other) noexcept
{
    // Stop the source's decode pipeline before transferring members —
    // the loop references its backends and the frame queue (which moves
    // address in this operation).
    other.teardownPipeline();

    _state = other._state;
    _lastResult = other._lastResult;
    _loopEnabled = other._loopEnabled;
    _rate = other._rate;
    _onStateChanged = std::move(other._onStateChanged);
    _onEndOfStream = std::move(other._onEndOfStream);
    _demuxer = std::move(other._demuxer);
    _decoder = std::move(other._decoder);
    _info = other._info;
    _path = std::move(other._path);
    _queue = std::move(other._queue);
    _loop = std::move(other._loop);
    _held = std::move(other._held);
    _presented = std::move(other._presented);
    _clock = std::move(other._clock);

    // The source is now an empty shell.
    other._state = PlayerState::Stopped;
}

AYVideoPlayer& AYVideoPlayer::operator=(AYVideoPlayer&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    teardownPipeline();
    other.teardownPipeline();

    _state = other._state;
    _lastResult = other._lastResult;
    _loopEnabled = other._loopEnabled;
    _rate = other._rate;
    _onStateChanged = std::move(other._onStateChanged);
    _onEndOfStream = std::move(other._onEndOfStream);
    _demuxer = std::move(other._demuxer);
    _decoder = std::move(other._decoder);
    _info = other._info;
    _path = std::move(other._path);
    _queue = std::move(other._queue);
    _loop = std::move(other._loop);
    _held = std::move(other._held);
    _presented = std::move(other._presented);
    _clock = std::move(other._clock);

    other._state = PlayerState::Stopped;
    return *this;
}

AYVideoPlayer::~AYVideoPlayer()
{
    teardownPipeline();
}

void AYVideoPlayer::notifyStateChanged(PlayerState state) noexcept
{
    if (_onStateChanged)
    {
        try
        {
            _onStateChanged(state);
        }
        catch (...)
        {
            // Callbacks must not throw (A-12); guard anyway so a
            // misbehaving callback cannot kill the player.
        }
    }
}

void AYVideoPlayer::notifyEndOfStream() noexcept
{
    if (_onEndOfStream)
    {
        try
        {
            _onEndOfStream();
        }
        catch (...)
        {
        }
    }
}

void AYVideoPlayer::setOnStateChanged(std::function<void(PlayerState)> cb) noexcept
{
    _onStateChanged = std::move(cb);
}

void AYVideoPlayer::setOnEndOfStream(std::function<void()> cb) noexcept
{
    _onEndOfStream = std::move(cb);
}

VideoResult AYVideoPlayer::transition(PlayerState from, PlayerState to) noexcept
{
    if (_state != from || !isTransitionLegal(from, to))
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    _state = to;
    notifyStateChanged(to);
    return VideoResult::Ok;
}

void AYVideoPlayer::startLoop()
{
    _loop = std::make_unique<DecodeLoop>(*_demuxer, *_decoder, *_queue);
    _loop->start();
}

void AYVideoPlayer::teardownPipeline() noexcept
{
    if (_loop)
    {
        _loop->requestStop();
        if (_queue)
        {
            _queue->clear(); // unblock a producer stuck in push (§8.3)
        }
        _loop->join();
        if (_queue)
        {
            _queue->clear(); // drop anything pushed between the cancels
        }
        _loop.reset();
    }
    _held.reset();
    _presented.reset();
}

VideoResult AYVideoPlayer::open(const std::string& path)
{
    if (_state != PlayerState::Idle && _state != PlayerState::Stopped)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    const PlayerState from = _state;
    transition(from, PlayerState::Opening);

    // A build without the FFmpeg package returns null backends from
    // makeFFmpeg* — report it instead of crashing (design.md §5.4).
    if (!_demuxer || !_decoder)
    {
        _lastResult = VideoResult::UnsupportedFormat;
        transition(PlayerState::Opening, PlayerState::Failed);
        return _lastResult;
    }

    DemuxerOpenParams params;
    params.path = path;
    if (auto r = _demuxer->open(params); r != VideoResult::Ok)
    {
        _lastResult = r;
        transition(PlayerState::Opening, PlayerState::Failed);
        return r;
    }
    if (auto r = _demuxer->getMediaInfo(_info); r != VideoResult::Ok)
    {
        _lastResult = r;
        transition(PlayerState::Opening, PlayerState::Failed);
        return r;
    }

    DecoderOpenParams decoderParams;
    decoderParams.codecName = _info.videoCodec;
    decoderParams.media = _info;
    if (auto r = _decoder->open(decoderParams); r != VideoResult::Ok)
    {
        _lastResult = r;
        transition(PlayerState::Opening, PlayerState::Failed);
        return r;
    }

    _path = path;
    if (_queue)
    {
        _queue->clear();
    }
    _held.reset();
    _presented.reset();
    _lastResult = VideoResult::Ok;
    transition(PlayerState::Opening, PlayerState::Ready);
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::play()
{
    if (_state == PlayerState::Ready)
    {
        // Fresh open or end-of-stream: replay from 0.
        if (_loop)
        {
            teardownPipeline();
        }
        if (_queue)
        {
            _queue->clear();
        }
        _held.reset();
        _presented.reset();
        if (_decoder)
        {
            _decoder->flush();
        }
        if (_demuxer)
        {
            (void)_demuxer->seek(ayt::time::Duration{});
        }
        _clock.reset(ayt::time::Duration{});
        startLoop();
        transition(PlayerState::Ready, PlayerState::Playing);
        return VideoResult::Ok;
    }
    if (_state == PlayerState::Paused)
    {
        if (_loop && _loop->finished())
        {
            // The whole stream decoded while paused (tiny streams):
            // resume from the paused position instead of 0.
            teardownPipeline();
            const ayt::time::Duration pos = _clock.position();
            if (_queue)
            {
                _queue->clear();
            }
            _held.reset();
            if (_decoder)
            {
                _decoder->flush();
            }
            if (_demuxer)
            {
                (void)_demuxer->seek(pos);
            }
            _clock.reset(pos);
            startLoop();
        }
        else
        {
            _clock.markResumed();
        }
        transition(PlayerState::Paused, PlayerState::Playing);
        return VideoResult::Ok;
    }
    _lastResult = VideoResult::InvalidState;
    return VideoResult::InvalidState;
}

VideoResult AYVideoPlayer::pause()
{
    if (_state == PlayerState::Playing)
    {
        _clock.markPaused();
        return transition(PlayerState::Playing, PlayerState::Paused);
    }
    // Ready/Paused/Seeking -> Paused is idempotent (skeleton contract).
    if (_state == PlayerState::Ready || _state == PlayerState::Paused
        || _state == PlayerState::Seeking)
    {
        _state = PlayerState::Paused;
        _lastResult = VideoResult::Ok;
        notifyStateChanged(PlayerState::Paused);
        return VideoResult::Ok;
    }
    _lastResult = VideoResult::InvalidState;
    return VideoResult::InvalidState;
}

VideoResult AYVideoPlayer::stop()
{
    const bool recoverable =
        _state == PlayerState::Ready || _state == PlayerState::Playing
        || _state == PlayerState::Paused || _state == PlayerState::Seeking
        || _state == PlayerState::Failed;
    if (!recoverable)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }

    teardownPipeline();
    if (_demuxer)
    {
        _demuxer->close();
    }
    if (_decoder)
    {
        _decoder->close();
    }
    _info = MediaInfo{};
    _path.clear();
    _clock.reset();
    transition(_state, PlayerState::Stopped);
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::seek(const ayt::time::Duration& target)
{
    const bool seekable =
        _state == PlayerState::Ready || _state == PlayerState::Playing
        || _state == PlayerState::Paused;
    if (!seekable)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }

    const PlayerState preSeek = _state;
    transition(preSeek, PlayerState::Seeking);

    // §8.3 flush sequence: stop the decode thread, drop queued frames,
    // reset the decoder, reposition the demuxer.
    teardownPipeline();
    if (_decoder)
    {
        _decoder->flush();
    }
    if (_demuxer)
    {
        if (auto r = _demuxer->seek(target); r != VideoResult::Ok)
        {
            _lastResult = r;
            transition(PlayerState::Seeking, PlayerState::Failed);
            return r;
        }
    }
    _clock.reset(target);

    if (preSeek == PlayerState::Playing)
    {
        startLoop();
        transition(PlayerState::Seeking, PlayerState::Playing);
    }
    else
    {
        transition(PlayerState::Seeking, preSeek);
    }
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

void AYVideoPlayer::setLoop(bool loop) noexcept
{
    _loopEnabled = loop;
}

bool AYVideoPlayer::isLooping() const noexcept
{
    return _loopEnabled;
}

VideoResult AYVideoPlayer::setRate(double rate) noexcept
{
    if (rate < 0.25 || rate > 4.0)
    {
        _lastResult = VideoResult::InvalidArgument;
        return VideoResult::InvalidArgument;
    }
    _rate = rate;
    (void)_clock.setRate(rate);
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

double AYVideoPlayer::rate() const noexcept
{
    return _rate;
}

PlayerState AYVideoPlayer::state() const noexcept
{
    return _state;
}

bool AYVideoPlayer::isPlaying() const noexcept
{
    return _state == PlayerState::Playing;
}

VideoResult AYVideoPlayer::getMediaInfo(MediaInfo& outInfo) const
{
    if (_state != PlayerState::Ready && _state != PlayerState::Playing
        && _state != PlayerState::Paused && _state != PlayerState::Seeking)
    {
        return VideoResult::InvalidState;
    }
    outInfo = _info;
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::pullFrame(VideoFrame& out)
{
    out = VideoFrame{};
    if (_state != PlayerState::Playing || !_queue)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }

    // 1) The held frame (pulled early, waiting for its pts).
    if (_held)
    {
        if (_held->frame.pts <= _clock.position())
        {
            _presented = std::move(_held);
            out = _presented->frame;
            out.data = _presented->pixels.data();
            return VideoResult::Ok;
        }
        return VideoResult::Ok; // Ok + null: not due yet (§6.2)
    }

    // 2) Next queued frame. Ownership moves into `_held` (or stays
    // there until the frame is due) so `out.data` never dangles when
    // the local QueuedFrame goes out of scope (§4.5).
    QueuedFrame qf;
    if (_queue->tryPop(qf))
    {
        _held = std::make_unique<QueuedFrame>(std::move(qf));
        if (_held->frame.pts <= _clock.position())
        {
            _presented = std::move(_held);
            out = _presented->frame;
            out.data = _presented->pixels.data();
            return VideoResult::Ok;
        }
        return VideoResult::Ok; // Ok + null: not due yet (§6.2)
    }

    // 3) Queue empty. Still decoding -> no frame ready.
    if (!_loop || !_loop->finished())
    {
        return VideoResult::Ok;
    }

    // Stream finished.
    const VideoResult failure = _loop->failure();
    if (failure != VideoResult::Ok)
    {
        _lastResult = failure;
        teardownPipeline();
        transition(PlayerState::Playing, PlayerState::Failed);
        return failure;
    }

    if (!_loop->endedCleanly())
    {
        // Ended without a clean EOS and without a failure: a cancelled
        // run. Only stop()/seek() cancel, and both leave Playing — this
        // state is transient; report no frame.
        return VideoResult::Ok;
    }

    // Clean end-of-stream (design.md §20 A-06). The decode thread is
    // finished; teardown joins it and empties the queue (already empty).
    teardownPipeline();

    if (_loopEnabled)
    {
        // Loop mode: restart silently from 0 (no EOS event, no state
        // change — the timeline restarts).
        if (_queue)
        {
            _queue->clear();
        }
        if (_decoder)
        {
            _decoder->flush();
        }
        if (_demuxer)
        {
            (void)_demuxer->seek(ayt::time::Duration{});
        }
        _clock.reset(ayt::time::Duration{});
        startLoop();
        return VideoResult::Ok;
    }

    // Non-loop: present the end once, then settle in Ready.
    _lastResult = VideoResult::Ok;
    transition(PlayerState::Playing, PlayerState::Ready);
    notifyEndOfStream();
    return VideoResult::EndOfStream;
}

VideoResult AYVideoPlayer::lastResult() const noexcept
{
    return _lastResult;
}

} // namespace ayt::video
