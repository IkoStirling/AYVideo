#include <AYVideo/VideoSubSystem.h>

#include <AYVideo/IVideoBackendFactory.h>

#include <AYAudio/AudioSubSystem.h>

namespace ayt::video
{

VideoSubSystem* VideoSubSystem::s_instance = nullptr;

VideoSubSystem::VideoSubSystem()
{
    s_instance = this;
    _descriptor.name = "Video";
    // After Audio (600) so streamPush/render order is stable (§15).
    _descriptor.basePriority = 650;
    _descriptor.timeType = ayt::game::SubSystemDescriptor::TimeType::Unscaled;
    _descriptor.dependencies = {"Audio"};
    _descriptor.phases = ayt::game::phaseBit(ayt::game::FramePhase::Presentation);
    _descriptor.clock = ayt::game::ClockDomain::Unscaled;
    _descriptor.runsAfter = {"Audio"};
    _descriptor.phasePriority = 650;
}

VideoSubSystem::~VideoSubSystem()
{
    shutdown();
    if (s_instance == this)
    {
        s_instance = nullptr;
    }
}

const ayt::game::SubSystemDescriptor& VideoSubSystem::getDescriptor() const
{
    return _descriptor;
}

void VideoSubSystem::setBackendFactory(BackendFactory factory)
{
    if (_initialized)
    {
        return;
    }
    _factory = std::move(factory);
}

std::pair<std::unique_ptr<IAYVideoDemuxer>,
          std::unique_ptr<IAYVideoDecoder>>
VideoSubSystem::makeBackends() const
{
    if (_factory)
    {
        return _factory();
    }
    return {makeFFmpegDemuxer(), makeFFmpegDecoder()};
}

bool VideoSubSystem::initialize()
{
    if (_initialized)
    {
        return true;
    }
    _slots.clear();
    _slots.resize(8); // small pool; mirrors Audio kMaxStreams spirit
    _initialized = true;
    return true;
}

void VideoSubSystem::shutdown()
{
    if (!_initialized)
    {
        return;
    }
    for (Slot& s : _slots)
    {
        if (s.inUse && s.player)
        {
            (void)s.player->stop();
        }
        s = Slot{};
    }
    _slots.clear();
    _initialized = false;
}

VideoSubSystem::Slot* VideoSubSystem::findSlot(VideoPlaybackId id) noexcept
{
    if (id == InvalidVideoPlayback)
    {
        return nullptr;
    }
    for (Slot& s : _slots)
    {
        if (s.inUse && s.id == id)
        {
            return &s;
        }
    }
    return nullptr;
}

const VideoSubSystem::Slot* VideoSubSystem::findSlot(VideoPlaybackId id) const noexcept
{
    if (id == InvalidVideoPlayback)
    {
        return nullptr;
    }
    for (const Slot& s : _slots)
    {
        if (s.inUse && s.id == id)
        {
            return &s;
        }
    }
    return nullptr;
}

VideoPlaybackId VideoSubSystem::play(const std::string& path, bool loop)
{
    if (!_initialized || path.empty())
    {
        return InvalidVideoPlayback;
    }

    Slot* free = nullptr;
    for (Slot& s : _slots)
    {
        if (!s.inUse)
        {
            free = &s;
            break;
        }
    }
    if (!free)
    {
        return InvalidVideoPlayback;
    }

    auto backends = makeBackends();
    if (!backends.first || !backends.second)
    {
        return InvalidVideoPlayback;
    }

    auto player = std::make_unique<AYVideoPlayer>(
        std::move(backends.first), std::move(backends.second), _now);

    // Bridge PCM when an AudioSubSystem engine is live (§11).
    if (auto* audio = ayt::audio::AudioSubSystem::findRegistered())
    {
        if (audio->engine())
        {
            (void)player->attachAudioEngine(audio->engine());
        }
    }

    if (player->open(path) != VideoResult::Ok)
    {
        return InvalidVideoPlayback;
    }
    player->setLoop(loop);
    if (player->play() != VideoResult::Ok)
    {
        (void)player->stop();
        return InvalidVideoPlayback;
    }

    const VideoPlaybackId id = _nextId++;
    free->id = id;
    free->player = std::move(player);
    free->framePixels.clear();
    free->frame = VideoFrame{};
    free->hasFrame = false;
    free->inUse = true;
    return id;
}

void VideoSubSystem::stop(VideoPlaybackId id)
{
    Slot* s = findSlot(id);
    if (!s)
    {
        return;
    }
    if (s->player)
    {
        (void)s->player->stop();
    }
    *s = Slot{};
}

void VideoSubSystem::setFrameTexture(VideoPlaybackId id,
                                     IVideoFrameTexture* texture) noexcept
{
    if (Slot* s = findSlot(id))
    {
        s->texture = texture;
    }
}

IVideoFrameTexture* VideoSubSystem::frameTexture(VideoPlaybackId id) noexcept
{
    Slot* s = findSlot(id);
    return s ? s->texture : nullptr;
}

void VideoSubSystem::pause(VideoPlaybackId id)
{
    if (Slot* s = findSlot(id); s && s->player)
    {
        (void)s->player->pause();
    }
}

void VideoSubSystem::resume(VideoPlaybackId id)
{
    if (Slot* s = findSlot(id); s && s->player)
    {
        (void)s->player->play();
    }
}

VideoPlaybackInfo VideoSubSystem::info(VideoPlaybackId id) const
{
    VideoPlaybackInfo out{};
    const Slot* s = findSlot(id);
    if (!s || !s->player)
    {
        return out;
    }
    out.id = id;
    out.state = s->player->state();
    out.hasFrame = s->hasFrame;
    out.position = s->player->position();
    return out;
}

AYVideoPlayer* VideoSubSystem::player(VideoPlaybackId id) noexcept
{
    Slot* s = findSlot(id);
    return s ? s->player.get() : nullptr;
}

bool VideoSubSystem::lastFrame(VideoPlaybackId id, VideoFrame& out) const
{
    const Slot* s = findSlot(id);
    if (!s || !s->hasFrame)
    {
        out = VideoFrame{};
        return false;
    }
    out = s->frame;
    out.data = s->framePixels.empty() ? nullptr : s->framePixels.data();
    return out.data != nullptr;
}

void VideoSubSystem::update(float /*deltaTime*/)
{
    if (!_initialized)
    {
        return;
    }
    for (Slot& s : _slots)
    {
        if (!s.inUse || !s.player || !s.player->isPlaying())
        {
            continue;
        }
        VideoFrame f{};
        const VideoResult r = s.player->pullFrame(f);
        if (r == VideoResult::Ok && f.data && f.dataSize > 0)
        {
            s.framePixels.assign(f.data, f.data + f.dataSize);
            s.frame = f;
            s.frame.data = s.framePixels.data();
            s.hasFrame = true;

            if (s.texture)
            {
                if (s.texture->updateFromFrame(s.frame) == VideoResult::Ok)
                {
                    if (_sink)
                    {
                        try
                        {
                            _sink->onVideoFrame(s.id, *s.texture);
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
        }
        else if (r == VideoResult::EndOfStream)
        {
            // Non-loop EOS: slot stays until stop(); hasFrame kept.
        }
    }
}

} // namespace ayt::video
