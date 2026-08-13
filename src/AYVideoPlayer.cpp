#include <AYVideoPlayer.h>

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

// State transition table (design.md §10.4). `from -> to` legality.
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
            || to == PlayerState::Stopped;
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
    std::unique_ptr<IAYVideoDecoder> decoder)
    : _demuxer(std::move(demuxer))
    , _decoder(std::move(decoder))
{
}

AYVideoPlayer::AYVideoPlayer(AYVideoPlayer&&) noexcept = default;
AYVideoPlayer& AYVideoPlayer::operator=(AYVideoPlayer&&) noexcept = default;
AYVideoPlayer::~AYVideoPlayer() = default;

VideoResult AYVideoPlayer::transition(PlayerState from, PlayerState to) noexcept
{
    if (_state != from || !isTransitionLegal(from, to))
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    _state = to;
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::open(const std::string& path)
{
    if (_state != PlayerState::Idle && _state != PlayerState::Stopped)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }
    _state = PlayerState::Opening;

    DemuxerOpenParams params;
    params.path = path;
    if (auto r = _demuxer->open(params); r != VideoResult::Ok)
    {
        _state = PlayerState::Failed;
        _lastResult = r;
        return r;
    }
    if (auto r = _demuxer->getMediaInfo(_info); r != VideoResult::Ok)
    {
        _state = PlayerState::Failed;
        _lastResult = r;
        return r;
    }

    DecoderOpenParams decoderParams;
    decoderParams.codecName = _info.videoCodec;
    decoderParams.media = _info;
    if (auto r = _decoder->open(decoderParams); r != VideoResult::Ok)
    {
        _state = PlayerState::Failed;
        _lastResult = r;
        return r;
    }

    _path = path;
    _state = PlayerState::Ready;
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::play()
{
    if (auto r = transition(PlayerState::Ready, PlayerState::Playing);
        r == VideoResult::Ok)
    {
        return VideoResult::Ok;
    }
    return transition(PlayerState::Paused, PlayerState::Playing);
}

VideoResult AYVideoPlayer::pause()
{
    if (_state == PlayerState::Playing)
    {
        return transition(PlayerState::Playing, PlayerState::Paused);
    }
    // Ready/Paused/Seeking -> Paused is idempotent in V0.5 (skeleton
    // contract: pausing a non-playing player is a no-op Ok).
    if (_state == PlayerState::Ready || _state == PlayerState::Paused
        || _state == PlayerState::Seeking)
    {
        _state = PlayerState::Paused;
        _lastResult = VideoResult::Ok;
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
    _state = PlayerState::Stopped;
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

VideoResult AYVideoPlayer::seek(const ayt::time::Duration& target)
{
    const bool seekable =
        _state == PlayerState::Ready || _state == PlayerState::Playing
        || _state == PlayerState::Paused || _state == PlayerState::Seeking;
    if (!seekable)
    {
        _lastResult = VideoResult::InvalidState;
        return VideoResult::InvalidState;
    }

    const PlayerState preSeek = _state;
    _state = PlayerState::Seeking;
    const VideoResult r = _demuxer->seek(target);
    if (r != VideoResult::Ok)
    {
        _state = PlayerState::Failed;
        _lastResult = r;
        return r;
    }

    // V0.5 skeleton: seek completes synchronously; the seek-in-flight
    // dwell (Seeking state) arrives with async open/seek in V1.
    _state = preSeek;
    _lastResult = VideoResult::Ok;
    return VideoResult::Ok;
}

void AYVideoPlayer::setLoop(bool loop) noexcept
{
    _loop = loop;
}

bool AYVideoPlayer::isLooping() const noexcept
{
    return _loop;
}

VideoResult AYVideoPlayer::setRate(double rate) noexcept
{
    if (rate < 0.25 || rate > 4.0)
    {
        _lastResult = VideoResult::InvalidArgument;
        return VideoResult::InvalidArgument;
    }
    _rate = rate;
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

VideoResult AYVideoPlayer::lastResult() const noexcept
{
    return _lastResult;
}

} // namespace ayt::video
