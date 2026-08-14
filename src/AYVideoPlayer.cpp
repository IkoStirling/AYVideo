#include <AYVideoPlayer.h>

#include "AudioQueue.h"
#include "DecodeLoop.h"
#include "FrameQueue.h"
#include "SeekLog.h"

#include <AYAudioEngine.h>
#include <AYAudioTypes.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

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
    _clockGated = other._clockGated;
    _pipelinePrimed = other._pipelinePrimed;
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
    other._clockGated = false;
    other._pipelinePrimed = false;
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
    _clockGated = other._clockGated;
    _pipelinePrimed = other._pipelinePrimed;
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
    other._clockGated = false;
    other._pipelinePrimed = false;
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

ayt::time::Duration AYVideoPlayer::mediaDuration() const noexcept
{
    if (_info.durationSec <= 0.0)
    {
        return {};
    }
    return ayt::time::Duration::fromUs(
        static_cast<std::int64_t>(_info.durationSec * 1'000'000.0));
}

void AYVideoPlayer::freezeClockAt(const ayt::time::Duration& mediaPos) noexcept
{
    _clock.reset(mediaPos);
    _clock.markPaused();
}

void AYVideoPlayer::clampClockToDuration() noexcept
{
    const ayt::time::Duration dur = mediaDuration();
    if (dur.toUs() <= 0)
    {
        return;
    }
    if (_clock.position() >= dur)
    {
        freezeClockAt(dur);
    }
}

void AYVideoPlayer::armClockGate() noexcept
{
    // Decode needs a moment after seek/play start. If the wall clock runs
    // during that gap, the UI advances while the picture stays frozen —
    // then late frames get dropped. Hold the clock until a presentable
    // frame is queued.
    _clockGated = true;
    _clock.markPaused();
    if (!_buffering)
    {
        _buffering = true;
        notifyBufferingChanged(true);
    }
}

bool AYVideoPlayer::tryReleaseClockGate() noexcept
{
    if (!_clockGated || !_queue)
    {
        return false;
    }

    // In-flight Keyframe/Accurate seek: ignore pre-apply drain. Taking a
    // stale queued frame here rewound the clock to the old pts (often
    // near t=0) right after the user scrubbed mid-file.
    if (_awaitingSeekPreview && _loop
        && _loop->seekAppliedSerial() < _awaitingSeekSerial)
    {
        // Drop only pre-generation frames; do not bulk-clear (races the
        // first post-apply push).
        QueuedFrame junk;
        while (_queue->tryPop(junk))
        {
            if (junk.seekSerial >= _awaitingSeekSerial)
            {
                // Should not happen before apply; keep it staged.
                _held = std::make_unique<QueuedFrame>(std::move(junk));
                break;
            }
        }
        if (_audioQueue)
        {
            QueuedAudio ajunk;
            while (_audioQueue->tryPop(ajunk))
            {
            }
        }
        return false;
    }

    // Drop anything below the seek floor, stage the first usable frame.
    // Do not wait for bufferHigh here — that delayed first paint by an
    // extra GOP's worth of decode on progressive HTTP seeks.
    // Pre-floor frames are discarded silently (DecodeLoop also drops
    // them before enqueue after Accurate seek — this is a safety net).
    while (!_held)
    {
        QueuedFrame qf;
        if (!_queue->tryPop(qf))
        {
            break;
        }
        if (qf.frame.pts < _minPresentPts)
        {
            continue;
        }
        // Reject frames from a previous seek generation (stale high-pts
        // pictures that survived postSeekMin).
        if (_awaitingSeekPreview
            && qf.seekSerial < _awaitingSeekSerial)
        {
            continue;
        }
        _held = std::make_unique<QueuedFrame>(std::move(qf));
    }
    while (_held && _held->frame.pts < _minPresentPts)
    {
        _held.reset();
        QueuedFrame qf;
        if (_queue->tryPop(qf))
        {
            if (qf.frame.pts >= _minPresentPts)
            {
                _held = std::make_unique<QueuedFrame>(std::move(qf));
            }
        }
        else
        {
            break;
        }
    }
    if (!_held)
    {
        return false;
    }

    // Anchor to the first presentable frame's pts so playback starts
    // in sync (no cold wall-clock run while picture was frozen).
    _clock.reset(_held->frame.pts);
    _clock.markResumed();
    _clockGated = false;
    if (_floorWaitStarted.time_since_epoch().count() != 0)
    {
        seeklog::stage("player.floor.ready",
                       seeklog::msSince(_floorWaitStarted));
        _floorWaitStarted = {};
    }
    if (_buffering)
    {
        _buffering = false;
        notifyBufferingChanged(false);
    }
    return true;
}

bool AYVideoPlayer::primeFirstFrame(bool anyKeyframe) noexcept
{
    // Decode-thread must be stopped. Pull packets until the first video
    // frame at/after the seek floor is ready — callers can present it
    // immediately instead of waiting on a cold DecodeLoop start.
    // `anyKeyframe`: take the first decoded frame (I-frame after demux
    // seek) for scrub preview — skip the costly keyframe→target walk.
    if (!_demuxer || !_decoder || !_demuxer->isOpen() || !_decoder->isOpen())
    {
        return false;
    }

    _held.reset();

    auto takeFrame = [&](const VideoFrame& frame) -> bool {
        if (!frame.data || frame.dataSize == 0)
        {
            return false;
        }
        if (!anyKeyframe && frame.pts < _minPresentPts)
        {
            return false; // V4 floor: keep decoding toward target
        }
        auto q = std::make_unique<QueuedFrame>();
        q->pixels.assign(frame.data, frame.data + frame.dataSize);
        q->frame = frame;
        q->frame.data = q->pixels.data();
        _held = std::move(q);
        return true;
    };

    auto drainVideo = [&]() -> bool {
        for (;;)
        {
            VideoFrame frame;
            const VideoResult fr = _decoder->dequeueFrame(frame);
            if (fr == VideoResult::Ok && frame.data)
            {
                if (takeFrame(frame))
                {
                    return true;
                }
                continue; // pre-floor — drop
            }
            if (fr == VideoResult::DecodeError)
            {
                continue;
            }
            return false; // Ok+null, EOS, or hard error
        }
    };

    auto drainAudio = [&]() {
        if (!_audioQueue)
        {
            return;
        }
        for (;;)
        {
            AudioPcmFrame audio;
            const VideoResult ar = _decoder->dequeueAudioFrame(audio);
            if (ar == VideoResult::Ok && audio.data && audio.frameCount > 0)
            {
                _audioQueue->push(audio);
                continue;
            }
            break;
        }
    };

    VideoPacket pkt;
    // Bound work so a broken stream cannot hang seek on the UI thread.
    constexpr int kMaxPackets = 600;
    for (int n = 0; n < kMaxPackets && !_held; ++n)
    {
        const VideoResult rr = _demuxer->readNextPacket(pkt);
        if (rr == VideoResult::EndOfStream)
        {
            break;
        }
        if (rr != VideoResult::Ok)
        {
            break;
        }

        for (;;)
        {
            const VideoResult fr = _decoder->feedPacket(pkt);
            if (fr == VideoResult::QueueFull)
            {
                if (drainVideo())
                {
                    return true;
                }
                drainAudio();
                continue;
            }
            if (fr == VideoResult::DecodeError)
            {
                break; // soft-skip this packet
            }
            if (fr != VideoResult::Ok)
            {
                return static_cast<bool>(_held);
            }
            if (drainVideo())
            {
                return true;
            }
            drainAudio();
            break;
        }
    }
    return static_cast<bool>(_held);
}

void AYVideoPlayer::updateBufferingFromQueue() noexcept
{
    if (_clockGated || !_networkStreaming || !_queue)
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
    if (_held)
    {
        // Sync-primed after seek: paint immediately, no clock gate.
        _clock.reset(_held->frame.pts);
        _clock.markResumed();
        _clockGated = false;
        if (_buffering)
        {
            _buffering = false;
            notifyBufferingChanged(false);
        }
    }
    else
    {
        armClockGate();
    }
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
    _clockGated = false;
    _pipelinePrimed = false;
    _awaitingSeekPreview = false;
    _awaitingSeekPreserveFloor = false;
    _seekPreviewNeedsClear = false;
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
        // V5 HTTP(S) progressive: enable seek when the server supports
        // Range (most static file servers do). DecodeLoop reconnect stays on.
        params.seekable = true;
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
        if (_demuxer)
        {
            _demuxer->clearAbort();
        }
        if (_demuxer && _demuxParams.seekable)
        {
            (void)_demuxer->seek(ayt::time::Duration{});
        }
        _minPresentPts = {};
        _pipelinePrimed = false;
        _clock.reset(ayt::time::Duration{});
        (void)applyActiveTracks();
        (void)ensureAudioBridge();
        startLoop(); // arms clock gate until first frame
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
            if (_pipelinePrimed)
            {
                // seek() already demux-seeked + primed `_held` — do not
                // flush/seek again (that doubled cold-start latency).
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
                    _loop.reset();
                }
                teardownAudioBridge();
                (void)applyActiveTracks();
                (void)ensureAudioBridge();
                startLoop();
                _pipelinePrimed = false;
            }
            else
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
                    _demuxer->clearAbort();
                }
                if (_demuxer && _demuxParams.seekable)
                {
                    (void)_demuxer->seek(pos);
                }
                teardownAudioBridge();
                _clock.reset(pos);
                (void)applyActiveTracks();
                (void)ensureAudioBridge();
                startLoop(); // arms clock gate
            }
        }
        else
        {
            if (_loop)
            {
                _loop->endScrubMode();
            }
            _awaitingSeekScrub = false;
            // Keep the scrub-target clock. Never re-anchor to a prior
            // Keyframe/Scrub picture (held.pts << clock) — that snapped
            // playback back to t≈0 after scrub-to-mid-GOP + resume.
            if (_awaitingSeekPreview)
            {
                armClockGate();
            }
            else if (_held && _held->frame.pts >= _minPresentPts)
            {
                _clock.reset(_held->frame.pts);
                _clock.markResumed();
                _clockGated = false;
            }
            else if (_minPresentPts.toUs() > 0)
            {
                _clock.reset(_minPresentPts);
                if (_held
                    && _held->frame.pts.toUs() + 80'000 < _minPresentPts.toUs()
                    && _loop && !_loop->finished())
                {
                    // Bitstream is still at the prior I-frame; drop
                    // keyframe→scrub-target at decode speed.
                    _loop->armDropBelow(_minPresentPts);
                    _presented = std::move(_held);
                    _held.reset();
                    if (_queue)
                    {
                        QueuedFrame junk;
                        while (_queue->tryPop(junk))
                        {
                            if (junk.frame.pts >= _minPresentPts
                                && junk.seekSerial >= _awaitingSeekSerial)
                            {
                                _held =
                                    std::make_unique<QueuedFrame>(std::move(junk));
                                break;
                            }
                        }
                    }
                    std::fprintf(stderr,
                                 "[AYVideo][seek] play.catchUp from KF "
                                 "floor=%.3fs\n",
                                 _minPresentPts.toUs() / 1e6);
                    std::fflush(stderr);
                }
                armClockGate();
            }
            else if (_held)
            {
                _clock.reset(_held->frame.pts);
                _clock.markResumed();
                _clockGated = false;
            }
            else
            {
                _clock.markResumed();
            }
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
    // reset() alone leaves the clock running — freeze at 0 so Stopped
    // position stays put (demo scrubber / position() queries).
    freezeClockAt({});
    transition(_state, PlayerState::Stopped);
    return VideoResult::Ok;
}

bool AYVideoPlayer::harvestSeekFrame(bool anyKeyframe,
                                     uint32_t timeoutMs) noexcept
{
    if (!_queue)
    {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    // Accurate: accept first frame with pts >= floor. A tight ceiling
    // (e.g. floor+80ms) discarded valid landings after DecodeLoop's
    // dropBelow already filtered pre-target frames — asyncFloor then
    // never completed.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    uint32_t dropped = 0;
    for (;;)
    {
        QueuedFrame qf;
        while (_queue->tryPop(qf))
        {
            if (qf.seekSerial < _awaitingSeekSerial)
            {
                // Pre-seek generation — pts alone cannot filter these
                // (stale frames often have *higher* pts than the new land).
                ++dropped;
                continue;
            }
            if (_loop)
            {
                const std::int64_t postMin = _loop->postSeekMinPtsUs();
                if (postMin == std::numeric_limits<std::int64_t>::max())
                {
                    // Seek applied but first post-seek packet not seen yet —
                    // anything in the queue is stale.
                    ++dropped;
                    continue;
                }
                if (postMin > 0 && qf.frame.pts.toUs() < postMin)
                {
                    ++dropped;
                    continue;
                }
            }
            if (!anyKeyframe && qf.frame.pts < _minPresentPts)
            {
                ++dropped;
                continue;
            }
            _held = std::make_unique<QueuedFrame>(std::move(qf));
            const double ms = seeklog::msSince(t0);
            seeklog::stage("player.harvest", ms);
            // Always print landing pts — diagnose "jumped to start" scrub bugs.
            {
                const double ptsSec = _held->frame.pts.toUs() / 1e6;
                const double floorSec = _minPresentPts.toUs() / 1e6;
                const double clockSec = _clock.position().toUs() / 1e6;
                // Keyframe scrub: picture at prior I-frame while clock stays
                // on the scrub thumb is expected for long-GOP files.
                const bool early = !anyKeyframe && clockSec > 1.0
                                   && ptsSec + 1.0 < clockSec;
                std::fprintf(stderr,
                             "[AYVideo][seek] player.harvest ok pts=%.3fs "
                             "clock=%.3fs floor=%.3fs dropped=%u serial=%llu%s\n",
                             ptsSec, clockSec, floorSec, dropped,
                             static_cast<unsigned long long>(_held->seekSerial),
                             early ? "  << LAND_EARLY" : "");
                std::fflush(stderr);
            }
            return true;
        }
        if (timeoutMs == 0)
        {
            return false;
        }
        if (_loop && _loop->finished())
        {
            seeklog::stage("player.harvest.fail.finished", seeklog::msSince(t0));
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            seeklog::stage("player.harvest.TIMEOUT", seeklog::msSince(t0));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool AYVideoPlayer::harvestScrubLatest() noexcept
{
    if (!_queue)
    {
        return false;
    }
    QueuedFrame best;
    bool got = false;
    uint32_t dropped = 0;
    QueuedFrame qf;
    while (_queue->tryPop(qf))
    {
        if (qf.seekSerial < _awaitingSeekSerial)
        {
            ++dropped;
            continue;
        }
        if (_loop)
        {
            const std::int64_t postMin = _loop->postSeekMinPtsUs();
            if (postMin == std::numeric_limits<std::int64_t>::max())
            {
                ++dropped;
                continue;
            }
            if (postMin > 0 && qf.frame.pts.toUs() < postMin)
            {
                ++dropped;
                continue;
            }
        }
        // Prefer the newest frame at/under the scrub clock; allow a small
        // lead so DropOldest cadence still paints.
        const std::int64_t clockUs = _clock.position().toUs();
        if (clockUs > 0 && qf.frame.pts.toUs() > clockUs + 80'000)
        {
            ++dropped;
            continue;
        }
        best = std::move(qf);
        got = true;
    }
    if (!got)
    {
        return false;
    }
    _held = std::make_unique<QueuedFrame>(std::move(best));
    if (seeklog::enabled())
    {
        std::fprintf(stderr,
                     "[AYVideo][seek] player.scrubLatest pts=%.3fs clock=%.3fs "
                     "dropped=%u serial=%llu\n",
                     _held->frame.pts.toUs() / 1e6, _clock.position().toUs() / 1e6,
                     dropped,
                     static_cast<unsigned long long>(_held->seekSerial));
        std::fflush(stderr);
    }
    return true;
}

bool AYVideoPlayer::postInLoopSeek(const ayt::time::Duration& target,
                                   SeekMode mode, bool waitApplied,
                                   bool waitFirstFrame) noexcept
{
    if (!_loop || _loop->finished() || !_queue)
    {
        return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (_held)
    {
        _presented = std::move(_held);
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

    InLoopSeekMode loopMode = InLoopSeekMode::Accurate;
    if (mode == SeekMode::Keyframe)
    {
        loopMode = InLoopSeekMode::Keyframe;
    }
    else if (mode == SeekMode::Scrub)
    {
        loopMode = InLoopSeekMode::Scrub;
    }

    const uint64_t serial =
        _loop->requestSeek(target, loopMode, vStream, aStream);
    // Arm the generation filter before any clear/wait/harvest so stale
    // queue entries (seekSerial < serial) cannot win the race.
    _awaitingSeekSerial = serial;
    _awaitingSeekKeyframe = (mode == SeekMode::Keyframe);
    _awaitingSeekScrub = (mode == SeekMode::Scrub);
    _queue->clear();
    if (_audioQueue)
    {
        _audioQueue->clear();
    }

    if (seeklog::enabled())
    {
        const char* modeName = "Accurate";
        if (mode == SeekMode::Keyframe)
        {
            modeName = "Keyframe";
        }
        else if (mode == SeekMode::Scrub)
        {
            modeName = "Scrub";
        }
        std::fprintf(stderr,
                     "[AYVideo][seek] player.postInLoop serial=%llu target=%.3fs "
                     "mode=%s waitApplied=%d waitFrame=%d\n",
                     static_cast<unsigned long long>(serial),
                     target.toUs() / 1e6, modeName, waitApplied ? 1 : 0,
                     waitFirstFrame ? 1 : 0);
        std::fflush(stderr);
    }

    if (waitApplied)
    {
        const auto tWait = std::chrono::steady_clock::now();
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(8000);
        while (_loop->seekAppliedSerial() < serial)
        {
            if (_loop->finished() || _loop->failure() != VideoResult::Ok)
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                seeklog::stage("player.waitApplied.TIMEOUT",
                               seeklog::msSince(tWait));
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        seeklog::stage("player.waitApplied", seeklog::msSince(tWait));
        if (_loop->seekAppliedSerial() < serial)
        {
            return false;
        }
        if (_loop->lastSeekResult() != VideoResult::Ok)
        {
            _lastResult = _loop->lastSeekResult();
            return false;
        }
    }

    if (waitFirstFrame)
    {
        bool got = false;
        if (mode == SeekMode::Keyframe || mode == SeekMode::Scrub)
        {
            got = (mode == SeekMode::Scrub)
                      ? harvestScrubLatest()
                      : harvestSeekFrame(/*anyKeyframe=*/true, 8000);
            if (got && _held && _held->frame.pts > _minPresentPts
                && mode == SeekMode::Keyframe)
            {
                _minPresentPts = _held->frame.pts;
            }
        }
        else
        {
            got = harvestSeekFrame(/*anyKeyframe=*/false, /*timeoutMs=*/0);
            if (!got)
            {
                if (_held && _held->frame.pts < _minPresentPts)
                {
                    _presented = std::move(_held);
                    _held.reset();
                }
                _awaitingSeekSerial = _loop->seekAppliedSerial();
                _awaitingSeekKeyframe = false;
                _awaitingSeekScrub = false;
                _awaitingSeekPreserveFloor = true;
                _awaitingSeekPreview = true;
                _seekPreviewNeedsClear = false;
                _pipelinePrimed = false;
                seeklog::stage("player.postInLoop.asyncFloor",
                               seeklog::msSince(t0));
                return true;
            }
        }
        if (!got)
        {
            seeklog::stage("player.postInLoop.FAIL", seeklog::msSince(t0));
            return false;
        }
        _pipelinePrimed = static_cast<bool>(_held);
        _awaitingSeekPreview = false;
        _awaitingSeekPreserveFloor = false;
        _seekPreviewNeedsClear = false;
        seeklog::stage("player.postInLoop.total", seeklog::msSince(t0));
        return true;
    }

    if (_held)
    {
        _presented = std::move(_held);
        _held.reset();
    }
    _awaitingSeekSerial = serial;
    _awaitingSeekKeyframe = (mode == SeekMode::Keyframe);
    _awaitingSeekScrub = (mode == SeekMode::Scrub);
    _awaitingSeekPreserveFloor = (mode == SeekMode::Accurate);
    _awaitingSeekPreview = true;
    _seekPreviewNeedsClear = false;
    _pipelinePrimed = false;
    if (mode == SeekMode::Accurate)
    {
        _floorWaitStarted = std::chrono::steady_clock::now();
    }
    seeklog::stage("player.postInLoop.async", seeklog::msSince(t0));
    return true;
}

void AYVideoPlayer::pollSeekPreview() noexcept
{
    if (!_awaitingSeekPreview || !_queue || !_loop)
    {
        return;
    }
    if (_loop->seekAppliedSerial() < _awaitingSeekSerial)
    {
        return;
    }
    if (_loop->lastSeekResult() != VideoResult::Ok)
    {
        _awaitingSeekPreview = false;
        _seekPreviewNeedsClear = false;
        return;
    }
    if (_seekPreviewNeedsClear)
    {
        _queue->clear();
        if (_audioQueue)
        {
            _audioQueue->clear();
        }
        _seekPreviewNeedsClear = false;
    }
    if (_awaitingSeekScrub)
    {
        if (harvestScrubLatest())
        {
            _pipelinePrimed = true;
            // Stay awaiting while scrub session runs so each tick picks up
            // newer frames; cleared on Accurate/Keyframe/play/stop.
        }
        return;
    }
    if (harvestSeekFrame(_awaitingSeekKeyframe, 0))
    {
        if (_awaitingSeekKeyframe && _held && !_awaitingSeekPreserveFloor)
        {
            // Keep scrub-target `_minPresentPts` for play() catch-up.
            // Rewriting it to the prior I-frame made resume snap to t≈0.
            if (_held->frame.pts > _minPresentPts)
            {
                _minPresentPts = _held->frame.pts;
            }
        }
        // Accurate: first snap may be a prior I-frame. Keep polling until
        // the floor frame arrives so paused scrub upgrades in place.
        if (_awaitingSeekPreserveFloor && _held
            && _held->frame.pts < _minPresentPts)
        {
            _awaitingSeekKeyframe = false;
            _pipelinePrimed = true;
            if (seeklog::enabled())
            {
                std::fprintf(stderr,
                             "[AYVideo][seek] player.pollPreview provisional "
                             "pts=%.3fs (waiting floor=%.3fs)\n",
                             _held->frame.pts.toUs() / 1e6,
                             _minPresentPts.toUs() / 1e6);
                std::fflush(stderr);
            }
            return;
        }
        _pipelinePrimed = true;
        _awaitingSeekPreview = false;
        _awaitingSeekPreserveFloor = false;
        if (_floorWaitStarted.time_since_epoch().count() != 0)
        {
            seeklog::stage("player.floor.ready",
                           seeklog::msSince(_floorWaitStarted));
            _floorWaitStarted = {};
        }
        if (seeklog::enabled())
        {
            std::fprintf(stderr,
                         "[AYVideo][seek] player.pollPreview harvested pts=%.3fs\n",
                         _held ? _held->frame.pts.toUs() / 1e6 : -1.0);
            std::fflush(stderr);
        }
    }
}

VideoResult AYVideoPlayer::seek(const ayt::time::Duration& target,
                                SeekMode mode)
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
        _lastResult = VideoResult::UnsupportedFormat;
        return VideoResult::UnsupportedFormat;
    }

    ayt::time::Duration clamped = target;
    if (clamped.toUs() < 0)
    {
        clamped = {};
    }
    const ayt::time::Duration dur = mediaDuration();
    if (dur.toUs() > 0 && clamped > dur)
    {
        clamped = dur;
    }

    const PlayerState preSeek = _state;
    const bool keyframePreview = (mode == SeekMode::Keyframe);
    const bool scrubPreview = (mode == SeekMode::Scrub);
    const bool loopAlive = _loop && !_loop->finished();
    const auto tSeek0 = std::chrono::steady_clock::now();
    const bool logAll = seeklog::enabled() || (!keyframePreview && !scrubPreview);
    if (logAll || scrubPreview)
    {
        const char* modeName = "Accurate";
        if (keyframePreview)
        {
            modeName = "Keyframe";
        }
        else if (scrubPreview)
        {
            modeName = "Scrub";
        }
        std::fprintf(stderr,
                     "[AYVideo][seek] ---- begin mode=%s target=%.3fs "
                     "state=%s loop=%d ----\n",
                     modeName, clamped.toUs() / 1e6, toString(preSeek),
                     loopAlive ? 1 : 0);
        std::fflush(stderr);
    }

    auto finishSeekLog = [&](const char* path) {
        const double totalMs = seeklog::msSince(tSeek0);
        seeklog::stage(path, totalMs);
        if (logAll || scrubPreview || totalMs >= 8.0)
        {
            std::fprintf(stderr,
                         "[AYVideo][seek] ---- end %s total=%.2f ms ----\n",
                         path, totalMs);
            std::fflush(stderr);
        }
    };

    if (loopAlive)
    {
        _clock.reset(clamped);
        setBuffering(false);

        if (scrubPreview)
        {
            _minPresentPts = clamped;
            _clock.markPaused();
            _clockGated = false;
            // Same GOP forward drag: only raise the decode ceiling.
            if (_loop->scrubActive() && _loop->updateScrubTarget(clamped))
            {
                _awaitingSeekScrub = true;
                _awaitingSeekPreview = true;
                _awaitingSeekKeyframe = false;
                _awaitingSeekPreserveFloor = false;
                if (preSeek == PlayerState::Playing)
                {
                    transition(PlayerState::Playing, PlayerState::Paused);
                }
                _lastResult = VideoResult::Ok;
                finishSeekLog("player.seek.inLoop.scrub.extend");
                return VideoResult::Ok;
            }
            if (_held)
            {
                _presented = std::move(_held);
                _held.reset();
            }
            if (postInLoopSeek(clamped, SeekMode::Scrub,
                               /*waitApplied=*/false,
                               /*waitFirstFrame=*/false))
            {
                if (preSeek == PlayerState::Playing)
                {
                    transition(PlayerState::Playing, PlayerState::Paused);
                }
                _lastResult = VideoResult::Ok;
                finishSeekLog("player.seek.inLoop.scrub.async");
                return VideoResult::Ok;
            }
            seeklog::event("player.seek fallback → sync Keyframe (scrub)");
        }
        else if (keyframePreview)
        {
            if (_held)
            {
                _presented = std::move(_held);
                _held.reset();
            }
            _minPresentPts = clamped;
            _clock.markPaused();
            _clockGated = false;
            if (_loop)
            {
                _loop->endScrubMode();
            }
            _awaitingSeekScrub = false;
            if (postInLoopSeek(clamped, SeekMode::Keyframe,
                               /*waitApplied=*/false,
                               /*waitFirstFrame=*/false))
            {
                if (preSeek == PlayerState::Playing)
                {
                    transition(PlayerState::Playing, PlayerState::Paused);
                }
                _lastResult = VideoResult::Ok;
                finishSeekLog("player.seek.inLoop.kf.async");
                return VideoResult::Ok;
            }
            seeklog::event("player.seek fallback → sync Keyframe");
        }
        else
        {
            transition(preSeek, PlayerState::Seeking);
            _minPresentPts = clamped;
            if (_loop)
            {
                _loop->endScrubMode();
            }
            _awaitingSeekScrub = false;
            if (postInLoopSeek(clamped, SeekMode::Accurate,
                               /*waitApplied=*/true,
                               /*waitFirstFrame=*/false))
            {
                if (preSeek == PlayerState::Playing)
                {
                    _clock.reset(clamped);
                    if (_held && _held->frame.pts >= _minPresentPts)
                    {
                        _clock.reset(_held->frame.pts);
                        _clock.markResumed();
                        _clockGated = false;
                    }
                    else
                    {
                        armClockGate();
                    }
                    transition(PlayerState::Seeking, PlayerState::Playing);
                }
                else
                {
                    _clock.markPaused();
                    transition(PlayerState::Seeking, preSeek);
                }
                _lastResult = VideoResult::Ok;
                finishSeekLog("player.seek.inLoop.accurate");
                return VideoResult::Ok;
            }
            seeklog::event("player.seek fallback → sync Accurate");
            transition(PlayerState::Seeking, preSeek);
        }
    }

    if (keyframePreview)
    {
        if (_held)
        {
            _presented = std::move(_held);
        }
        _clockGated = false;
        _pipelinePrimed = false;
        _awaitingSeekPreview = false;
        _awaitingSeekPreserveFloor = false;
        _seekPreviewNeedsClear = false;
        setBuffering(false);

        if (_decoder)
        {
            _decoder->flush();
        }
        if (_demuxer)
        {
            _demuxer->clearAbort();
            const auto tDs = std::chrono::steady_clock::now();
            if (auto r = _demuxer->seek(clamped); r != VideoResult::Ok)
            {
                seeklog::stage("player.sync.kf.demuxSeek.FAIL",
                               seeklog::msSince(tDs));
                _lastResult = r;
                freezeClockAt(clamped);
                if (preSeek == PlayerState::Playing)
                {
                    _clock.markPaused();
                    transition(preSeek, PlayerState::Paused);
                }
                finishSeekLog("player.seek.sync.kf.FAIL");
                return r;
            }
            seeklog::stage("player.sync.kf.demuxSeek", seeklog::msSince(tDs));
        }

        _minPresentPts = clamped; // resume floor (KF picture may lag)
        _clock.reset(clamped);
        _clock.markPaused();
        const auto tPrime = std::chrono::steady_clock::now();
        (void)primeFirstFrame(/*anyKeyframe=*/true);
        seeklog::stage("player.sync.prime.kf", seeklog::msSince(tPrime));
        if (_held)
        {
            _pipelinePrimed = true;
            if (_held->frame.pts > _minPresentPts)
            {
                _minPresentPts = _held->frame.pts;
            }
        }

        if (preSeek == PlayerState::Playing)
        {
            transition(PlayerState::Playing, PlayerState::Paused);
        }
        _lastResult = VideoResult::Ok;
        finishSeekLog("player.seek.sync.kf");
        return VideoResult::Ok;
    }

    transition(preSeek, PlayerState::Seeking);

    const auto tTear = std::chrono::steady_clock::now();
    teardownPipeline();
    teardownAudioBridge();
    seeklog::stage("player.sync.teardown", seeklog::msSince(tTear));
    if (_decoder)
    {
        _decoder->flush();
    }
    if (_demuxer)
    {
        _demuxer->clearAbort();
    }
    if (_demuxer)
    {
        const auto tDs = std::chrono::steady_clock::now();
        if (auto r = _demuxer->seek(clamped); r != VideoResult::Ok)
        {
            seeklog::stage("player.sync.demuxSeek.FAIL", seeklog::msSince(tDs));
            _lastResult = r;
            freezeClockAt(clamped);
            transition(PlayerState::Seeking, preSeek == PlayerState::Playing
                                                 ? PlayerState::Paused
                                                 : preSeek);
            finishSeekLog("player.seek.sync.accurate.FAIL");
            return r;
        }
        seeklog::stage("player.sync.demuxSeek", seeklog::msSince(tDs));
    }

    _clock.reset(clamped);
    (void)applyActiveTracks();
    _minPresentPts = clamped;
    const auto tPrime = std::chrono::steady_clock::now();
    (void)primeFirstFrame(/*anyKeyframe=*/false);
    seeklog::stage("player.sync.prime", seeklog::msSince(tPrime));
    _pipelinePrimed = true;

    if (preSeek == PlayerState::Playing)
    {
        (void)ensureAudioBridge();
        startLoop();
        _pipelinePrimed = false;
        if (_audioEngine)
        {
            _audioEngine->resume();
        }
        transition(PlayerState::Seeking, PlayerState::Playing);
    }
    else
    {
        _clock.markPaused();
        transition(PlayerState::Seeking, preSeek);
    }
    _lastResult = VideoResult::Ok;
    finishSeekLog("player.seek.sync.accurate");
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

VideoResult AYVideoPlayer::currentFrame(VideoFrame& out) noexcept
{
    out = VideoFrame{};
    if (_state != PlayerState::Ready && _state != PlayerState::Paused
        && _state != PlayerState::Playing)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    pollSeekPreview();
    // Prefer the freshest scrub prime (`_held`), else last presented.
    const QueuedFrame* src = _held ? _held.get() : _presented.get();
    if (!src || !src->frame.data || src->pixels.empty())
    {
        _lastResult = VideoResult::Ok;
        return VideoResult::Ok; // Ok + null — nothing staged yet
    }
    out = src->frame;
    out.data = src->pixels.data();
    _lastResult = VideoResult::Ok;
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

    // After seek/play start: hold the clock until the first presentable
    // frame arrives (avoids "time moves, picture stuck" then a hitch).
    if (_awaitingSeekPreview)
    {
        pollSeekPreview();
    }
    if (_clockGated)
    {
        (void)tryReleaseClockGate();
        if (_clockGated)
        {
            if (!_loop || !_loop->finished())
            {
                return VideoResult::Ok; // waiting for decode
            }
            // Loop died while gated — fall through to failure/EOS handling.
        }
        else if (presentDueFrame(out))
        {
            // Released this tick — present immediately at the anchored pts.
            return VideoResult::Ok;
        }
    }

    // V5: refresh buffering before presentation decisions.
    updateBufferingFromQueue();
    // Freeze at media end so the UI clock cannot run past duration while
    // the decode thread is still flushing / after the last frame.
    if (!_buffering)
    {
        clampClockToDuration();
    }
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
            freezeClockAt(mediaDuration().toUs() > 0 ? mediaDuration()
                                                      : _clock.position());
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
        return VideoResult::Ok;
    }

    teardownAudioBridge();
    _lastResult = VideoResult::Ok;
    setBuffering(false);
    freezeClockAt(mediaDuration().toUs() > 0 ? mediaDuration()
                                              : _clock.position());
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
