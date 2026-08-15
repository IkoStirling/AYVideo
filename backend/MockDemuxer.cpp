#include "MockDemuxer.h"

#include <AYTime/Duration.h>

#include <cstring>

namespace ayt::video
{

namespace
{

// Deterministic synthetic payload: 16 bytes of 0x41 + packet index.
void fillPayload(std::vector<uint8_t>& payload, int32_t index)
{
    payload.resize(16);
    for (size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<uint8_t>(0x41u + static_cast<uint8_t>(i));
    }
    payload[0] = static_cast<uint8_t>(index & 0xFF);
}

} // namespace

MockDemuxer::MockDemuxer(int32_t packetCount)
    : _packetCount(packetCount < 0 ? 0 : packetCount)
{
    fillPayload(_payload, 0);
}

VideoResult MockDemuxer::open(const DemuxerOpenParams& params)
{
    ++_openCount;
    if (_failOpen)
    {
        _open = false;
        return VideoResult::DemuxError;
    }
    _params = params;
    _open = true;
    _emitted = 0;
    _closed = false;
    _disconnected = false;
    _activeVideoStream = 0;
    _activeAudioStream = _provideMultiAudio ? 1 : -1;
    _activeSubtitleStream = -1;
    _subtitleCueEmitMask = 0;
    return VideoResult::Ok;
}

void MockDemuxer::close() noexcept
{
    _open = false;
    _closed = true;
}

bool MockDemuxer::isOpen() const noexcept
{
    return _open;
}

VideoResult MockDemuxer::getMediaInfo(MediaInfo& outInfo) const
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    // Synthetic 320x240 @ 25 fps video-only stream; duration derived
    // from the packet count so EOS timing is coherent.
    outInfo = MediaInfo{};
    outInfo.width = 320;
    outInfo.height = 240;
    outInfo.frameRate = 25.0;
    outInfo.durationSec = static_cast<double>(_packetCount) / 25.0;
    outInfo.hasVideo = true;
    outInfo.videoCodec = "mock-h264";
    {
        VideoTrackInfo vt{};
        vt.streamIndex = 0;
        vt.codec = "mock-h264";
        vt.width = 320;
        vt.height = 240;
        vt.frameRate = 25.0;
        outInfo.videoTracks.push_back(vt);
    }
    if (_provideMultiAudio)
    {
        AudioTrackInfo eng{};
        eng.streamIndex = 1;
        eng.codec = "mock-aac";
        eng.language = "eng";
        eng.title = "English";
        eng.sampleRate = 48000;
        eng.channels = 2;
        AudioTrackInfo jpn = eng;
        jpn.streamIndex = 2;
        jpn.language = "jpn";
        jpn.title = "Japanese";
        outInfo.audioTracks.push_back(eng);
        outInfo.audioTracks.push_back(jpn);
        outInfo.hasAudio = true;
        const AudioTrackInfo& active =
            (_activeAudioStream == 2) ? jpn : eng;
        outInfo.audioCodec = active.codec;
        outInfo.audioSampleRate = active.sampleRate;
        outInfo.audioChannels = active.channels;
    }
    if (_provideSubtitle)
    {
        SubtitleTrackInfo sub{};
        sub.streamIndex = 3;
        sub.kind = SubtitleKind::Text;
        sub.codec = "mock-srt";
        sub.language = "eng";
        sub.title = "Mock English";
        outInfo.subtitleTracks.push_back(sub);
        outInfo.hasSubtitles = true;
        // Two fixed soft cues for CI (200 ms windows @ 25 fps timeline).
        SubtitleCue c0{};
        c0.start = ayt::time::Duration::fromUs(0);
        c0.end = ayt::time::Duration::fromUs(200'000);
        c0.text = "mock-cue-0";
        SubtitleCue c1{};
        c1.start = ayt::time::Duration::fromUs(200'000);
        c1.end = ayt::time::Duration::fromUs(400'000);
        c1.text = "mock-cue-1";
        outInfo.softSubtitleCues.push_back(std::move(c0));
        outInfo.softSubtitleCues.push_back(std::move(c1));
    }
    return VideoResult::Ok;
}

VideoResult MockDemuxer::setActiveStreamIndices(int32_t videoStreamIndex,
                                                 int32_t audioStreamIndex)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    if (videoStreamIndex != 0)
    {
        return VideoResult::InvalidArgument;
    }
    _activeVideoStream = videoStreamIndex;
    if (_provideMultiAudio)
    {
        if (audioStreamIndex != 1 && audioStreamIndex != 2 && audioStreamIndex != -1)
        {
            return VideoResult::InvalidArgument;
        }
        _activeAudioStream = audioStreamIndex;
    }
    else if (audioStreamIndex != -1)
    {
        return VideoResult::InvalidArgument;
    }
    else
    {
        _activeAudioStream = -1;
    }
    return VideoResult::Ok;
}

VideoResult MockDemuxer::setActiveSubtitleStreamIndex(int32_t streamIndex)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    if (streamIndex < -1)
    {
        return VideoResult::InvalidArgument;
    }
    if (streamIndex >= 0)
    {
        if (!_provideSubtitle || streamIndex != 3)
        {
            return VideoResult::InvalidArgument;
        }
    }
    _activeSubtitleStream = streamIndex;
    _subtitleCueEmitMask = 0;
    return VideoResult::Ok;
}

VideoResult MockDemuxer::readNextPacket(VideoPacket& outPacket)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    if (_disconnected)
    {
        ++_readCount;
        return VideoResult::DemuxError;
    }
    if (_disconnectAfter >= 0 && _emitted >= _disconnectAfter)
    {
        _disconnected = true;
        ++_readCount;
        return VideoResult::DemuxError;
    }

    // Emit soft-subtitle packets at cue starts before the matching video
    // packet (streamIndex 3). Payload = UTF-8 text; duration = cue window.
    if (_provideSubtitle && _activeSubtitleStream == 3)
    {
        const std::int64_t nowUs =
            static_cast<std::int64_t>(_emitted) * 40'000;
        struct SoftCue
        {
            std::int64_t startUs;
            std::int64_t endUs;
            const char* text;
            uint32_t bit;
        };
        const SoftCue cues[] = {
            {0, 200'000, "mock-cue-0", 1u},
            {200'000, 400'000, "mock-cue-1", 2u},
        };
        for (const SoftCue& c : cues)
        {
            if ((_subtitleCueEmitMask & c.bit) != 0)
            {
                continue;
            }
            if (_emitted < _packetCount && nowUs == c.startUs)
            {
                _subtitleCueEmitMask |= c.bit;
                const auto* bytes =
                    reinterpret_cast<const uint8_t*>(c.text);
                const uint32_t len =
                    static_cast<uint32_t>(std::strlen(c.text));
                _payload.assign(bytes, bytes + len);
                outPacket = VideoPacket{};
                outPacket.data = _payload.data();
                outPacket.size = len;
                outPacket.isVideo = false;
                outPacket.isSubtitle = true;
                outPacket.streamIndex = 3;
                outPacket.pts = ayt::time::Duration::fromUs(c.startUs);
                outPacket.dts = outPacket.pts;
                outPacket.duration =
                    ayt::time::Duration::fromUs(c.endUs - c.startUs);
                ++_readCount;
                return VideoResult::Ok;
            }
        }
    }

    if (_emitted >= _packetCount)
    {
        return VideoResult::EndOfStream;
    }

    if (!_failReadDone && _failReadAt >= 0 && _emitted == _failReadAt)
    {
        _failReadDone = true;
        ++_readCount;
        return VideoResult::DemuxError;
    }

    fillPayload(_payload, _emitted);
    outPacket = VideoPacket{};
    outPacket.data = _payload.data();
    outPacket.size = static_cast<uint32_t>(_payload.size());
    outPacket.isVideo = true;
    outPacket.isSubtitle = false;
    outPacket.streamIndex = 0;
    outPacket.pts = ayt::time::Duration::fromUs(
        static_cast<std::int64_t>(_emitted) * 40'000); // 25 fps = 40 ms
    outPacket.dts = outPacket.pts;
    ++_emitted;
    ++_readCount;
    return VideoResult::Ok;
}

VideoResult MockDemuxer::seek(const ayt::time::Duration& target,
                              bool /*keyframeOnly*/)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    if (!_params.seekable)
    {
        return VideoResult::UnsupportedFormat;
    }
    ++_seekCount;
    // Jump the synthetic cursor to the first packet at/after target
    // (25 fps = 40 ms). Clamped to [0, packetCount].
    std::int64_t idx = target.toUs() / 40'000;
    if (idx < 0)
    {
        idx = 0;
    }
    if (idx > _packetCount)
    {
        idx = _packetCount;
    }
    _emitted = static_cast<int32_t>(idx);
    // Re-arm subtitle emits for cues at/after the new cursor.
    _subtitleCueEmitMask = 0;
    return VideoResult::Ok;
}

VideoResult MockDemuxer::reconnect()
{
    if (_params.path.empty() && !_open && _openCount == 0)
    {
        return VideoResult::NotInitialized;
    }
    ++_reconnectCount;
    if (_failReconnectRemaining > 0)
    {
        --_failReconnectRemaining;
        return VideoResult::DemuxError;
    }
    _disconnected = false;
    // One-shot latch: clear so the same disconnectAfter does not
    // immediately re-trip after a successful reconnect.
    _disconnectAfter = -1;
    // Keep packet cursor so playback resumes after the disconnect point.
    _open = true;
    _closed = false;
    return VideoResult::Ok;
}

} // namespace ayt::video
