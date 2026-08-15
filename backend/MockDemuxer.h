#pragma once
// MockDemuxer.h — scripted demuxer backend (V0.5).
//
// design.md §17: Mock backends MUST ship in V0.5. Semantics: open() is a
// no-op Ok; readNextPacket() replays `packetCount` synthetic video
// packets (deterministic payload pattern) then EndOfStream. Interaction
// counters let tests assert the player↔demuxer contract (design.md §19).

#include <IAYVideoDemuxer.h>

#include <cstdint>
#include <vector>

namespace ayt::video
{

class MockDemuxer final : public IAYVideoDemuxer
{
public:
    explicit MockDemuxer(int32_t packetCount);

    // V4 soft-subtitle discovery: when true, getMediaInfo reports one
    // Text track (codec=mock-srt, lang=eng). Default off so existing
    // Mock fixtures stay subtitle-free.
    void setProvideSubtitleTrack(bool on) noexcept { _provideSubtitle = on; }
    bool provideSubtitleTrack() const noexcept { return _provideSubtitle; }

    // V4 N-10: inject a second audio track (eng + jpn) for selection UTs.
    void setProvideMultiAudio(bool on) noexcept { _provideMultiAudio = on; }
    bool provideMultiAudio() const noexcept { return _provideMultiAudio; }

    // Active stream indices after setActiveStreamIndices (tests).
    int32_t activeVideoStreamIndex() const noexcept { return _activeVideoStream; }
    int32_t activeAudioStreamIndex() const noexcept { return _activeAudioStream; }
    int32_t activeSubtitleStreamIndex() const noexcept
    {
        return _activeSubtitleStream;
    }

    // Last successful open params (V5 network / ABR tests).
    const DemuxerOpenParams& lastOpenParams() const noexcept { return _params; }

    // Fault injection (design.md §19): when enabled, open() returns
    // DemuxError — drives the player's Failed state.
    void setFailOpen(bool fail) noexcept { _failOpen = fail; }
    bool failOpen() const noexcept { return _failOpen; }

    // V4 soft-skip: next read that would emit packet `index` returns
    // DemuxError once, then the same index is emitted on the following
    // read (packet is not consumed by the error).
    void failReadAt(int32_t index) noexcept
    {
        _failReadAt = index;
        _failReadDone = false;
    }

    // V5: after emitting `index` packets, subsequent reads return
    // DemuxError until reconnect() clears the disconnect latch.
    void disconnectAfter(int32_t index) noexcept
    {
        _disconnectAfter = index;
        _disconnected = false;
    }
    // Next N reconnect() calls fail (DemuxError); then succeed again.
    void failNextReconnects(uint32_t count) noexcept
    {
        _failReconnectRemaining = count;
    }
    uint32_t reconnectCount() const noexcept { return _reconnectCount; }

    // Test observers (design.md §19): interaction counters.
    uint32_t openCount() const noexcept { return _openCount; }
    uint32_t readCount() const noexcept { return _readCount; }
    uint32_t seekCount() const noexcept { return _seekCount; }
    bool wasClosed() const noexcept { return _closed; }

    VideoResult open(const DemuxerOpenParams& params) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    VideoResult getMediaInfo(MediaInfo& outInfo) const override;
    VideoResult readNextPacket(VideoPacket& outPacket) override;
    VideoResult seek(const ayt::time::Duration& target,
                     bool keyframeOnly = true) override;
    VideoResult setActiveStreamIndices(int32_t videoStreamIndex,
                                       int32_t audioStreamIndex) override;
    VideoResult setActiveSubtitleStreamIndex(int32_t streamIndex) override;
    VideoResult reconnect() override;

private:
    int32_t _packetCount = 0;
    int32_t _emitted = 0;
    bool _open = false;
    bool _closed = false;
    bool _failOpen = false;
    bool _provideSubtitle = false;
    bool _provideMultiAudio = false;
    int32_t _activeVideoStream = 0;
    int32_t _activeAudioStream = -1;
    int32_t _activeSubtitleStream = -1;
    uint32_t _subtitleCueEmitMask = 0; // bit i = cue i already emitted this pass
    int32_t _failReadAt = -1;
    bool _failReadDone = false;
    int32_t _disconnectAfter = -1;
    bool _disconnected = false;
    uint32_t _failReconnectRemaining = 0;
    DemuxerOpenParams _params{};

    uint32_t _openCount = 0;
    uint32_t _readCount = 0;
    uint32_t _seekCount = 0;
    uint32_t _reconnectCount = 0;

    // Synthetic packet payload storage (stable address for the packet
    // data pointer contract).
    std::vector<uint8_t> _payload;
};

} // namespace ayt::video
