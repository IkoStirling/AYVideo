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
    VideoResult seek(const ayt::time::Duration& target) override;

private:
    int32_t _packetCount = 0;
    int32_t _emitted = 0;
    bool _open = false;
    bool _closed = false;
    bool _failOpen = false;
    int32_t _failReadAt = -1;
    bool _failReadDone = false;

    uint32_t _openCount = 0;
    uint32_t _readCount = 0;
    uint32_t _seekCount = 0;

    // Synthetic packet payload storage (stable address for the packet
    // data pointer contract).
    std::vector<uint8_t> _payload;
};

} // namespace ayt::video
