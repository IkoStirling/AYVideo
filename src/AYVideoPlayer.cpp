#include <AYVideoPlayer.h>

#include "AudioQueue.h"
#include "DecodeLoop.h"
#include "FrameQueue.h"

#include <AYAudioEngine.h>
#include <AYAudioTypes.h>

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
            || to == PlayerState::Stopped || to == PlayerState::Ready
            || to == PlayerState::Failed;
    case PlayerState::Paused:
        return to == PlayerState::Playing || to == PlayerState::Seeking
            || to == PlayerState::Stopped;
    case PlayerState::Seeking:
        return to == PlayerState::Ready || to == PlayerState::Playing
            || to == PlayerState::Paused || to == PlayerState::Stopped
            || to == PlayerState::Failed;
    case PlayerState::Stopped:
        return to == PlayerState::Opening;
    case PlayerState::Failed:
        return to == PlayerState::Stopped;
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
    , _audioQueue(std::make_unique<AudioQueue>())
    , _clock(now)
{
    _queue->setOverflowPolicy(_frameOverflowPolicy);
}

AYVideoPlayer::AYVideoPlayer(AYVideoPlayer&& other) noexcept
{
    other.teardownPipeline();
    other.teardownAudioBridge();

    _state = other._state;
    _lastResult = other._lastResult;
    _loopEnabled = other._loopEnabled;
    _rate = other._rate;
    _onStateChanged = std::move(other._onStateChanged);
    _onEndOfStream = std::move(other._onEndOfStream);
    _onBufferingChanged = std::move(other._onBufferingChanged);
    _demuxer = std::move(other._demuxer);
    _decoder = std::move(other._decoder);
    _info = other._info;
    _path = std::move(other._path);
    _queue = std::move(other._queue);
    _audioQueue = std::move(other._audioQueue);
    _loop = std::move(other._loop);
    _held = std::move(other._held);
    _presented = std::move(other._presented);
    _clock = std::move(other._clock);
    _minPresentPts = other._minPresentPts;
    _activeSubtitleTrack = other._activeSubtitleTrack;
    _activeVideoTrack = other._activeVideoTrack;
    _activeAudioTrack = other._activeAudioTrack;
    _frameOverflowPolicy = other._frameOverflowPolicy;
    _demuxParams = other._demuxParams;
    _networkStreaming = other._networkStreaming;
    _buffering = other._buffering;
    _bufferLow = other._bufferLow;
    _bufferHigh = other._bufferHigh;
    _audioEngine = other._audioEngine;
    _audioStreamId = other._audioStreamId;
    _audioVoice = other._audioVoice;

    other._audioEngine = nullptr;
    other._audioStreamId = 0;
    other._audioVoice = 0;
    other._activeSubtitleTrack = -1;
    other._activeVideoTrack = 0;
    other._activeAudioTrack = 0;
    other._networkStreaming = false;
    other._buffering = false;
    other._state = PlayerState::Stopped;
}

AYVideoPlayer& AYVideoPlayer::operator=(AYVideoPlayer&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    teardownPipeline();
    teardownAudioBridge();
    other.teardownPipeline();
    other.teardownAudioBridge();

    _state = other._state;
    _lastResult = other._lastResult;
    _loopEnabled = other._loopEnabled;
    _rate = other._rate;
    _onStateChanged = std::move(other._onStateChanged);
    _onEndOfStream = std::move(other._onEndOfStream);
    _onBufferingChanged = std::move(other._onBufferingChanged);
    _demuxer = std::move(other._demuxer);
    _decoder = std::move(other._decoder);
    _info = other._info;
    _path = std::move(other._path);
    _queue = std::move(other._queue);
    _audioQueue = std::move(other._audioQueue);
    _loop = std::move(other._loop);
    _held = std::move(other._held);
    _presented = std::move(other._presented);
    _clock = std::move(other._clock);
    _minPresentPts = other._minPresentPts;
    _activeSubtitleTrack = other._activeSubtitleTrack;
    _activeVideoTrack = other._activeVideoTrack;
    _activeAudioTrack = other._activeAudioTrack;
    _frameOverflowPolicy = other._frameOverflowPolicy;
    _demuxParams = other._demuxParams;
    _networkStreaming = other._networkStreaming;
    _buffering = other._buffering;
    _bufferLow = other._bufferLow;
    _bufferHigh = other._bufferHigh;
    _audioEngine = other._audioEngine;
    _audioStreamId = other._audioStreamId;
    _audioVoice = other._audioVoice;

    other._audioEngine = nullptr;
    other._audioStreamId = 0;
    other._audioVoice = 0;
    other._activeSubtitleTrack = -1;
    other._activeVideoTrack = 0;
    other._activeAudioTrack = 0;
    other._networkStreaming = false;
    other._buffering = false;
    other._state = PlayerState::Stopped;
    return *this;
}

AYVideoPlayer::~AYVideoPlayer()
{
    teardownPipeline();
    teardownAudioBridge();
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

void AYVideoPlayer::setOnBufferingChanged(std::function<void(bool)> cb) noexcept
{
    _onBufferingChanged = std::move(cb);
}

bool AYVideoPlayer::isBuffering() const noexcept
{
    return _buffering;
}

void AYVideoPlayer::setBufferWatermarks(uint32_t low, uint32_t high) noexcept
{
    _bufferLow = low;
    _bufferHigh = high < low ? low : high;
}

void AYVideoPlayer::notifyBufferingChanged(bool buffering) noexcept
{
    if (_onBufferingChanged)
    {
        try
        {
            _onBufferingChanged(buffering);
        }
        catch (...)
        {
        }
    }
}

void AYVideoPlayer::setBuffering(bool buffering) noexcept
{
    if (_buffering == buffering)
    {
        return;
    }
    _buffering = buffering;
    if (buffering)
    {
        _clock.markPaused();
    }
    else
    {
        _clock.markResumed();
    }
    notifyBufferingChanged(buffering);
}

void AYVideoPlayer::updateBufferingFromQueue() noexcept
{
    if (!_networkStreaming || !_queue)
    {
        return;
    }
    uint32_t size = _queue->size();
    if (_held)
    {
        ++size;
    }
    if (!_buffering && size <= _bufferLow)
    {
        setBuffering(true);
    }
    else if (_buffering && size >= _bufferHigh)
    {
        setBuffering(false);
    }
}

VideoResult AYVideoPlayer::attachAudioEngine(ayt::audio::AudioEngine* engine) noexcept
{
    if (_state != PlayerState::Idle && _state != PlayerState::Stopped)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    teardownAudioBridge();
    _audioEngine = engine;
    if (_audioEngine)
    {
        _clock.setAudioMasterProvider(&AYVideoPlayer::audioMasterThunk, this);
    }
    else
    {
        _clock.setAudioMasterProvider(nullptr, nullptr);
        (void)_clock.setSource(SyncSource::EngineClock);
    }
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

ayt::time::Duration AYVideoPlayer::audioMasterThunk(void* user) noexcept
{
    auto* self = static_cast<AYVideoPlayer*>(user);
    if (!self || !self->_audioEngine || self->_audioVoice == 0
        || self->_audioStreamId == 0)
    {
        return {};
    }
    const uint64_t frames = self->_audioEngine->voicePositionFrames(
        self->_audioVoice);
    const auto& desc = self->_audioEngine->streamDesc(
        static_cast<ayt::audio::AudioStreamId>(self->_audioStreamId));
    const uint32_t rate = desc.sampleRate > 0 ? desc.sampleRate : 48000u;
    const double us = static_cast<double>(frames) * 1'000'000.0
                      / static_cast<double>(rate);
    return ayt::time::Duration::fromUs(static_cast<std::int64_t>(us));
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

void AYVideoPlayer::teardownAudioBridge() noexcept
{
    if (_audioEngine)
    {
        if (_audioVoice != 0)
        {
            _audioEngine->stop(static_cast<ayt::audio::VoiceHandle>(_audioVoice));
            _audioVoice = 0;
        }
        if (_audioStreamId != 0)
        {
            _audioEngine->closeStream(
                static_cast<ayt::audio::AudioStreamId>(_audioStreamId));
            _audioStreamId = 0;
        }
    }
    else
    {
        _audioVoice = 0;
        _audioStreamId = 0;
    }
    (void)_clock.setSource(SyncSource::EngineClock);
}

bool AYVideoPlayer::ensureAudioBridge() noexcept
{
    if (!_audioEngine || !_info.hasAudio)
    {
        (void)_clock.setSource(SyncSource::EngineClock);
        return false;
    }
    if (_audioStreamId != 0 && _audioVoice != 0)
    {
        (void)_clock.setSource(SyncSource::AudioMaster);
        return true;
    }
    teardownAudioBridge();

    ayt::audio::AudioStreamDesc desc{};
    desc.channels = 2;
    desc.sampleRate = 48000;
    desc.capacityFrames = 24000; // ~0.5 s @ 48 kHz (§11 / §16.3)
    const auto id = _audioEngine->openStream(desc);
    if (id == ayt::audio::InvalidStreamId)
    {
        return false;
    }
    _audioStreamId = id;
    const auto voice = _audioEngine->playStream(
        id, ayt::audio::AudioBus::Sfx, {}, 1.0f, false);
    if (voice == ayt::audio::InvalidVoice)
    {
        _audioEngine->closeStream(id);
        _audioStreamId = 0;
        return false;
    }
    _audioVoice = voice;
    _clock.setAudioMasterProvider(&AYVideoPlayer::audioMasterThunk, this);
    (void)_clock.setSource(SyncSource::AudioMaster);
    return true;
}

void AYVideoPlayer::pumpAudioToEngine() noexcept
{
    if (!_audioEngine || _audioStreamId == 0 || !_audioQueue)
    {
        return;
    }
    QueuedAudio qa;
    while (_audioQueue->tryPop(qa))
    {
        if (!qa.frame.data || qa.frame.frameCount == 0)
        {
            continue;
        }
        (void)_audioEngine->streamPush(
            static_cast<ayt::audio::AudioStreamId>(_audioStreamId),
            qa.frame.data,
            qa.frame.frameCount);
    }
}

bool AYVideoPlayer::presentDueFrame(VideoFrame& out)
{
    const ayt::time::Duration pos = _clock.position();
    const ayt::time::Duration tol = _clock.driftTolerance();

    auto isLate = [&](const ayt::time::Duration& pts) {
        return pts + tol < pos;
    };
    // V4: after keyframe seek, drop pre-target frames before they can
    // present (clock is already at `target`, so isLate alone still lets
    // through frames within one driftTolerance of the target).
    auto isBeforeSeekFloor = [&](const ayt::time::Duration& pts) {
        return pts < _minPresentPts;
    };
    auto shouldDrop = [&](const ayt::time::Duration& pts) {
        return isLate(pts) || isBeforeSeekFloor(pts);
    };
    auto isDue = [&](const ayt::time::Duration& pts) {
        return pts <= pos;
    };

    // Drop late / pre-seek frames (audio-master drift §9.2 + V4 floor).
    while (_held && shouldDrop(_held->frame.pts))
    {
        _held.reset();
        QueuedFrame qf;
        if (_queue && _queue->tryPop(qf))
        {
            _held = std::make_unique<QueuedFrame>(std::move(qf));
        }
        else
        {
            break;
        }
    }

    if (_held)
    {
        if (isDue(_held->frame.pts))
        {
            _presented = std::move(_held);
            out = _presented->frame;
            out.data = _presented->pixels.data();
            return true;
        }
        return false; // early — wait
    }

    QueuedFrame qf;
    if (_queue && _queue->tryPop(qf))
    {
        _held = std::make_unique<QueuedFrame>(std::move(qf));
        while (_held && shouldDrop(_held->frame.pts))
        {
            _held.reset();
            QueuedFrame next;
            if (_queue->tryPop(next))
            {
                _held = std::make_unique<QueuedFrame>(std::move(next));
            }
            else
            {
                break;
            }
        }
        if (_held && isDue(_held->frame.pts))
        {
            _presented = std::move(_held);
            out = _presented->frame;
            out.data = _presented->pixels.data();
            return true;
        }
    }
    return false;
}

void AYVideoPlayer::startLoop()
{
    AudioQueue* aq = (_audioEngine && _info.hasAudio) ? _audioQueue.get() : nullptr;
    DecodeLoopOptions opts;
    opts.reconnectMax = _demuxParams.reconnectMax;
    opts.reconnectDelayMs = _demuxParams.reconnectDelayMs;
    _loop = std::make_unique<DecodeLoop>(*_demuxer, *_decoder, *_queue, aq, opts);
    _loop->start();
}

void AYVideoPlayer::teardownPipeline() noexcept
{
    if (_loop)
    {
        _loop->requestStop();
        if (_queue)
        {
            _queue->clear();
        }
        if (_audioQueue)
        {
            _audioQueue->clear();
        }
        _loop->join();
        if (_queue)
        {
            _queue->clear();
        }
        if (_audioQueue)
        {
            _audioQueue->clear();
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

    if (!_demuxer || !_decoder)
    {
        _lastResult = VideoResult::UnsupportedFormat;
        transition(PlayerState::Opening, PlayerState::Failed);
        return _lastResult;
    }

    DemuxerOpenParams params;
    params.path = path;
    if (isHttpUrl(path))
    {
        // V5 HTTP(S) progressive: non-seekable + DecodeLoop reconnect.
        params.seekable = false;
        params.reconnectMax = 3;
        params.reconnectDelayMs = 50; // keep Mock inject tests snappy
    }
    if (auto r = _demuxer->open(params); r != VideoResult::Ok)
    {
        _lastResult = r;
        transition(PlayerState::Opening, PlayerState::Failed);
        return r;
    }
    _demuxParams = params;
    _networkStreaming = isHttpUrl(path) || params.reconnectMax > 0;
    setBuffering(false);
    if (auto r = _demuxer->getMediaInfo(_info); r != VideoResult::Ok)
    {
        _lastResult = r;
        transition(PlayerState::Opening, PlayerState::Failed);
        return r;
    }

    DecoderOpenParams decoderParams;
    decoderParams.codecName = _info.videoCodec;
    decoderParams.media = _info;
    decoderParams.decodeAudio = (_audioEngine != nullptr) && _info.hasAudio;
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
    if (_audioQueue)
    {
        _audioQueue->clear();
    }
    _held.reset();
    _presented.reset();
    _minPresentPts = {};
    _activeSubtitleTrack = -1;
    _activeVideoTrack = _info.videoTracks.empty() ? -1 : 0;
    _activeAudioTrack = _info.audioTracks.empty() ? -1 : 0;
    _lastResult = VideoResult::Ok;
    transition(PlayerState::Opening, PlayerState::Ready);
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::play()
{
    if (_state == PlayerState::Ready)
    {
        if (_loop)
        {
            teardownPipeline();
        }
        teardownAudioBridge();
        if (_queue)
        {
            _queue->clear();
        }
        if (_audioQueue)
        {
            _audioQueue->clear();
        }
        _held.reset();
        _presented.reset();
        if (_decoder)
        {
            _decoder->flush();
        }
        if (_demuxer && _demuxParams.seekable)
        {
            (void)_demuxer->seek(ayt::time::Duration{});
        }
        _minPresentPts = {};
        _clock.reset(ayt::time::Duration{});
        (void)applyActiveTracks();
        (void)ensureAudioBridge();
        startLoop();
        if (_networkStreaming)
        {
            // Start in buffering until the high watermark fills.
            setBuffering(true);
        }
        if (_audioEngine)
        {
            _audioEngine->resume();
        }
        transition(PlayerState::Ready, PlayerState::Playing);
        return VideoResult::Ok;
    }
    if (_state == PlayerState::Paused)
    {
        // Restart when the loop is gone (seek-while-paused tears it
        // down) or already finished — resume at the current clock
        // position so V4 seek floors survive into play().
        if (!_loop || _loop->finished())
        {
            teardownPipeline();
            const ayt::time::Duration pos = _clock.position();
            if (_queue)
            {
                _queue->clear();
            }
            if (_audioQueue)
            {
                _audioQueue->clear();
            }
            _held.reset();
            if (_decoder)
            {
                _decoder->flush();
            }
            if (_demuxer && _demuxParams.seekable)
            {
                (void)_demuxer->seek(pos);
            }
            teardownAudioBridge();
            _clock.reset(pos);
            (void)applyActiveTracks();
            (void)ensureAudioBridge();
            startLoop();
            if (_networkStreaming)
            {
                setBuffering(true);
            }
        }
        else
        {
            _clock.markResumed();
        }
        if (_audioEngine)
        {
            _audioEngine->resume();
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
        if (_audioEngine)
        {
            _audioEngine->pause();
        }
        return transition(PlayerState::Playing, PlayerState::Paused);
    }
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
    teardownAudioBridge();
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
    _minPresentPts = {};
    _activeSubtitleTrack = -1;
    _activeVideoTrack = 0;
    _activeAudioTrack = 0;
    _demuxParams = {};
    _networkStreaming = false;
    setBuffering(false);
    _clock.reset();
    transition(_state, PlayerState::Stopped);
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::seek(const ayt::time::Duration& target)
{
    const bool seekableState =
        _state == PlayerState::Ready || _state == PlayerState::Playing
        || _state == PlayerState::Paused;
    if (!seekableState)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    if (!_demuxParams.seekable)
    {
        // V5: HTTP progressive defaults to non-seekable.
        _lastResult = VideoResult::UnsupportedFormat;
        return VideoResult::UnsupportedFormat;
    }

    const PlayerState preSeek = _state;
    transition(preSeek, PlayerState::Seeking);

    teardownPipeline();
    teardownAudioBridge();
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
    // V4: floor presentation at `target` (keyframe seek may rewind;
    // presentDueFrame discards pts < floor).
    _minPresentPts = target;
    _clock.reset(target);

    if (preSeek == PlayerState::Playing)
    {
        (void)applyActiveTracks();
        (void)ensureAudioBridge();
        startLoop();
        if (_audioEngine)
        {
            _audioEngine->resume();
        }
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

uint32_t AYVideoPlayer::subtitleTrackCount() const noexcept
{
    return static_cast<uint32_t>(_info.subtitleTracks.size());
}

VideoResult AYVideoPlayer::getSubtitleTrack(uint32_t index,
                                             SubtitleTrackInfo& out) const
{
    if (_state != PlayerState::Ready && _state != PlayerState::Playing
        && _state != PlayerState::Paused && _state != PlayerState::Seeking)
    {
        return VideoResult::InvalidState;
    }
    if (index >= _info.subtitleTracks.size())
    {
        return VideoResult::InvalidArgument;
    }
    out = _info.subtitleTracks[index];
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::setActiveSubtitleTrack(int32_t index) noexcept
{
    if (_state != PlayerState::Ready && _state != PlayerState::Playing
        && _state != PlayerState::Paused && _state != PlayerState::Seeking)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    if (index < -1
        || index >= static_cast<int32_t>(_info.subtitleTracks.size()))
    {
        _lastResult = VideoResult::InvalidArgument;
        return VideoResult::InvalidArgument;
    }
    _activeSubtitleTrack = index;
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

int32_t AYVideoPlayer::activeSubtitleTrack() const noexcept
{
    return _activeSubtitleTrack;
}

uint32_t AYVideoPlayer::videoTrackCount() const noexcept
{
    return static_cast<uint32_t>(_info.videoTracks.size());
}

uint32_t AYVideoPlayer::audioTrackCount() const noexcept
{
    return static_cast<uint32_t>(_info.audioTracks.size());
}

VideoResult AYVideoPlayer::getVideoTrack(uint32_t index, VideoTrackInfo& out) const
{
    if (_state != PlayerState::Ready && _state != PlayerState::Playing
        && _state != PlayerState::Paused && _state != PlayerState::Seeking)
    {
        return VideoResult::InvalidState;
    }
    if (index >= _info.videoTracks.size())
    {
        return VideoResult::InvalidArgument;
    }
    out = _info.videoTracks[index];
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::getAudioTrack(uint32_t index, AudioTrackInfo& out) const
{
    if (_state != PlayerState::Ready && _state != PlayerState::Playing
        && _state != PlayerState::Paused && _state != PlayerState::Seeking)
    {
        return VideoResult::InvalidState;
    }
    if (index >= _info.audioTracks.size())
    {
        return VideoResult::InvalidArgument;
    }
    out = _info.audioTracks[index];
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::setActiveVideoTrack(int32_t index) noexcept
{
    if (_state != PlayerState::Ready && _state != PlayerState::Playing
        && _state != PlayerState::Paused && _state != PlayerState::Seeking)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    if (index < 0
        || index >= static_cast<int32_t>(_info.videoTracks.size()))
    {
        _lastResult = VideoResult::InvalidArgument;
        return VideoResult::InvalidArgument;
    }
    _activeVideoTrack = index;
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::setActiveAudioTrack(int32_t index) noexcept
{
    if (_state != PlayerState::Ready && _state != PlayerState::Playing
        && _state != PlayerState::Paused && _state != PlayerState::Seeking)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    if (index < -1
        || index >= static_cast<int32_t>(_info.audioTracks.size()))
    {
        _lastResult = VideoResult::InvalidArgument;
        return VideoResult::InvalidArgument;
    }
    _activeAudioTrack = index;
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

int32_t AYVideoPlayer::activeVideoTrack() const noexcept
{
    return _activeVideoTrack;
}

int32_t AYVideoPlayer::activeAudioTrack() const noexcept
{
    return _activeAudioTrack;
}

VideoResult AYVideoPlayer::applyActiveTracks() noexcept
{
    if (!_demuxer)
    {
        return VideoResult::NotInitialized;
    }
    int32_t vStream = -1;
    int32_t aStream = -1;
    if (_activeVideoTrack >= 0
        && _activeVideoTrack < static_cast<int32_t>(_info.videoTracks.size()))
    {
        vStream = _info.videoTracks[static_cast<size_t>(_activeVideoTrack)]
                      .streamIndex;
    }
    if (_activeAudioTrack >= 0
        && _activeAudioTrack < static_cast<int32_t>(_info.audioTracks.size()))
    {
        aStream = _info.audioTracks[static_cast<size_t>(_activeAudioTrack)]
                      .streamIndex;
    }
    if (vStream < 0)
    {
        return VideoResult::StreamNotFound;
    }
    const VideoResult r = _demuxer->setActiveStreamIndices(vStream, aStream);
    if (r != VideoResult::Ok)
    {
        return r;
    }
    // Refresh scalar MediaInfo from the remapped active streams.
    MediaInfo refreshed;
    if (_demuxer->getMediaInfo(refreshed) == VideoResult::Ok)
    {
        // Preserve track lists (selection indices stay valid).
        refreshed.videoTracks = _info.videoTracks;
        refreshed.audioTracks = _info.audioTracks;
        refreshed.subtitleTracks = _info.subtitleTracks;
        refreshed.hasSubtitles = _info.hasSubtitles;
        _info = std::move(refreshed);
    }
    return VideoResult::Ok;
}

ayt::time::Duration AYVideoPlayer::position() const noexcept
{
    return _clock.position();
}

SyncSource AYVideoPlayer::syncSource() const noexcept
{
    return _clock.source();
}

VideoResult AYVideoPlayer::pullFrame(VideoFrame& out)
{
    out = VideoFrame{};
    if (_state != PlayerState::Playing || !_queue)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }

    // Push any decoded PCM into AYAudio before consulting the clock so
    // AudioMaster position reflects freshly rendered audio (§11).
    pumpAudioToEngine();
    if (_audioEngine)
    {
        _audioEngine->submitFrame(0.0f);
    }

    // V5: refresh buffering before presentation decisions.
    updateBufferingFromQueue();
    if (_buffering)
    {
        // Hold the clock; still allow EOS / failure detection below.
        if (!_loop || !_loop->finished())
        {
            return VideoResult::Ok; // Ok + null while buffering
        }
    }
    else if (presentDueFrame(out))
    {
        updateBufferingFromQueue();
        return VideoResult::Ok;
    }
    if (_held)
    {
        return VideoResult::Ok; // early — Ok + null
    }

    if (!_loop || !_loop->finished())
    {
        return VideoResult::Ok;
    }

    const VideoResult failure = _loop->failure();
    if (failure != VideoResult::Ok)
    {
        _lastResult = failure;
        teardownPipeline();
        teardownAudioBridge();
        setBuffering(false);
        transition(PlayerState::Playing, PlayerState::Failed);
        return failure;
    }

    if (!_loop->endedCleanly())
    {
        return VideoResult::Ok;
    }

    teardownPipeline();
    if (_audioEngine && _audioStreamId != 0)
    {
        _audioEngine->markStreamEndOfStream(
            static_cast<ayt::audio::AudioStreamId>(_audioStreamId));
    }

    if (_loopEnabled)
    {
        if (!_demuxParams.seekable)
        {
            // Progressive streams cannot restart from 0 without seek.
            teardownAudioBridge();
            _lastResult = VideoResult::Ok;
            setBuffering(false);
            transition(PlayerState::Playing, PlayerState::Ready);
            notifyEndOfStream();
            return VideoResult::EndOfStream;
        }
        if (_queue)
        {
            _queue->clear();
        }
        if (_audioQueue)
        {
            _audioQueue->clear();
        }
        teardownAudioBridge();
        if (_decoder)
        {
            _decoder->flush();
        }
        if (_demuxer)
        {
            (void)_demuxer->seek(ayt::time::Duration{});
        }
        _clock.reset(ayt::time::Duration{});
        (void)ensureAudioBridge();
        startLoop();
        if (_networkStreaming)
        {
            setBuffering(true);
        }
        return VideoResult::Ok;
    }

    teardownAudioBridge();
    _lastResult = VideoResult::Ok;
    setBuffering(false);
    transition(PlayerState::Playing, PlayerState::Ready);
    notifyEndOfStream();
    return VideoResult::EndOfStream;
}

VideoResult AYVideoPlayer::lastResult() const noexcept
{
    return _lastResult;
}

AYVideoPlayer::QueueStats AYVideoPlayer::queueStats() const noexcept
{
    QueueStats s;
    if (_queue)
    {
        s.videoSize = _queue->size();
        s.videoCapacity = _queue->capacity();
        s.videoDropped = _queue->dropped();
    }
    if (_audioQueue)
    {
        s.audioSize = _audioQueue->size();
        s.audioCapacity = _audioQueue->capacity();
        s.audioDropped = _audioQueue->dropped();
    }
    return s;
}

VideoResult AYVideoPlayer::setFrameQueueOverflowPolicy(
    FrameQueueOverflowPolicy policy) noexcept
{
    if (_state == PlayerState::Playing || _state == PlayerState::Seeking)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    _frameOverflowPolicy = policy;
    if (_queue)
    {
        _queue->setOverflowPolicy(policy);
    }
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

} // namespace ayt::video
