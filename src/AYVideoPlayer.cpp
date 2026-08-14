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
    _audioEngine = other._audioEngine;
    _audioStreamId = other._audioStreamId;
    _audioVoice = other._audioVoice;

    other._audioEngine = nullptr;
    other._audioStreamId = 0;
    other._audioVoice = 0;
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
    _audioEngine = other._audioEngine;
    _audioStreamId = other._audioStreamId;
    _audioVoice = other._audioVoice;

    other._audioEngine = nullptr;
    other._audioStreamId = 0;
    other._audioVoice = 0;
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
    auto isDue = [&](const ayt::time::Duration& pts) {
        return pts <= pos;
    };

    // Drop late frames (audio-master drift correction, §9.2).
    while (_held && isLate(_held->frame.pts))
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
        while (_held && isLate(_held->frame.pts))
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
    _loop = std::make_unique<DecodeLoop>(*_demuxer, *_decoder, *_queue, aq);
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
        if (_demuxer)
        {
            (void)_demuxer->seek(ayt::time::Duration{});
        }
        _clock.reset(ayt::time::Duration{});
        (void)ensureAudioBridge();
        startLoop();
        if (_audioEngine)
        {
            _audioEngine->resume();
        }
        transition(PlayerState::Ready, PlayerState::Playing);
        return VideoResult::Ok;
    }
    if (_state == PlayerState::Paused)
    {
        if (_loop && _loop->finished())
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
            if (_demuxer)
            {
                (void)_demuxer->seek(pos);
            }
            teardownAudioBridge();
            _clock.reset(pos);
            (void)ensureAudioBridge();
            startLoop();
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
    _clock.reset(target);

    if (preSeek == PlayerState::Playing)
    {
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

    if (presentDueFrame(out))
    {
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
        return VideoResult::Ok;
    }

    teardownAudioBridge();
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
