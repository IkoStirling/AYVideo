#include "MockDemuxer.h"

#include <aytime/Duration.h>

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

VideoResult MockDemuxer::open(const DemuxerOpenParams& /*params*/)
{
    ++_openCount;
    if (_failOpen)
    {
        _open = false;
        return VideoResult::DemuxError;
    }
    _open = true;
    _emitted = 0;
    _closed = false;
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
    return VideoResult::Ok;
}

VideoResult MockDemuxer::readNextPacket(VideoPacket& outPacket)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
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
    outPacket.streamIndex = 0;
    outPacket.pts = ayt::time::Duration::fromUs(
        static_cast<std::int64_t>(_emitted) * 40'000); // 25 fps = 40 ms
    outPacket.dts = outPacket.pts;
    ++_emitted;
    ++_readCount;
    return VideoResult::Ok;
}

VideoResult MockDemuxer::seek(const ayt::time::Duration& /*target*/)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    ++_seekCount;
    _emitted = 0; // next read restarts from the beginning (skeleton contract)
    return VideoResult::Ok;
}

} // namespace ayt::video
