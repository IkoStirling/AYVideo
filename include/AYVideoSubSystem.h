#pragma once
// AYVideoSubSystem.h — Game-loop subsystem for AYVideo (design.md §15).
//
// Mirrors AYAudio AudioSubSystem: owns playback slots, pumps pullFrame
// each update, optionally bridges PCM through AudioSubSystem's engine.
// Host registers explicitly (static-lib REGISTER_SUBSYSTEM is dropped by
// MSVC unless the TU is referenced — same Audio pattern).
//
// V2+ scope: subsystem lifecycle + multi-slot play/stop + position
// query. Entity VideoComponent binding is a later cross-module PR
// (component POD already lives in ecs/VideoComponent.h).

#include <AYVideoFrame.h>
#include <AYVideoPlayer.h>
#include <AYVideoTypes.h>
#include <IAYVideoDecoder.h>
#include <IAYVideoDemuxer.h>
#include <aytime/Duration.h>

#include <AYGameLoop.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ayt::video
{

using VideoPlaybackId = uint32_t;
constexpr VideoPlaybackId InvalidVideoPlayback = 0;

struct VideoPlaybackInfo
{
    VideoPlaybackId id = InvalidVideoPlayback;
    PlayerState state = PlayerState::Idle;
    ayt::time::Duration position{};
    bool hasFrame = false;
};

class VideoSubSystem : public ayt::game::ISubSystem
{
public:
    VideoSubSystem();
    ~VideoSubSystem() override;

    static VideoSubSystem* findRegistered() { return s_instance; }

    const char* getName() const override { return "Video"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;

    // Optional: override demuxer/decoder construction (tests inject Mock).
    // Default constructs FFmpeg backends when AYVIDEO_HAS_FFMPEG, else Null.
    using BackendFactory = std::function<
        std::pair<std::unique_ptr<IAYVideoDemuxer>,
                  std::unique_ptr<IAYVideoDecoder>>()>;
    void setBackendFactory(BackendFactory factory);

    // Optional NowFn for SyncClock (tests inject FakeNow).
    void setNowFn(NowFn now) noexcept { _now = now; }

    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float /*fixedDeltaTime*/) override {}
    void shutdown() override;

    // Open + play. Returns InvalidVideoPlayback on failure.
    VideoPlaybackId play(const std::string& path, bool loop = false);

    void stop(VideoPlaybackId id);
    void pause(VideoPlaybackId id);
    void resume(VideoPlaybackId id);

    VideoPlaybackInfo info(VideoPlaybackId id) const;
    AYVideoPlayer* player(VideoPlaybackId id) noexcept;

    // Last frame presented by update() for this slot. data valid until
    // the next update()/stop() on the same id.
    bool lastFrame(VideoPlaybackId id, VideoFrame& out) const;

private:
    struct Slot
    {
        VideoPlaybackId id = InvalidVideoPlayback;
        std::unique_ptr<AYVideoPlayer> player;
        std::vector<uint8_t> framePixels;
        VideoFrame frame{};
        bool hasFrame = false;
        bool inUse = false;
    };

    Slot* findSlot(VideoPlaybackId id) noexcept;
    const Slot* findSlot(VideoPlaybackId id) const noexcept;
    std::pair<std::unique_ptr<IAYVideoDemuxer>,
              std::unique_ptr<IAYVideoDecoder>> makeBackends() const;

    BackendFactory _factory;
    NowFn _now = nullptr;
    ayt::game::SubSystemDescriptor _descriptor{};
    std::vector<Slot> _slots;
    VideoPlaybackId _nextId = 1;
    bool _initialized = false;

    static VideoSubSystem* s_instance;
};

} // namespace ayt::video
