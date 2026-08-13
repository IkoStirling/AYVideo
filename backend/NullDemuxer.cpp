#include "NullDemuxer.h"

namespace ayt::video
{

VideoResult NullDemuxer::open(const DemuxerOpenParams& /*params*/)
{
    // Null semantics: report Ok, remember the open state so the
    // isOpen()/getMediaInfo() contract stays coherent.
    _open = true;
    return VideoResult::Ok;
}

void NullDemuxer::close() noexcept
{
    _open = false;
}

bool NullDemuxer::isOpen() const noexcept
{
    return _open;
}

VideoResult NullDemuxer::getMediaInfo(MediaInfo& outInfo) const
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    outInfo = MediaInfo{};
    return VideoResult::Ok;
}

VideoResult NullDemuxer::readNextPacket(VideoPacket& /*outPacket*/)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    return VideoResult::EndOfStream;
}

VideoResult NullDemuxer::seek(const ayt::time::Duration& /*target*/)
{
    if (!_open)
    {
        return VideoResult::NotInitialized;
    }
    return VideoResult::Ok;
}

} // namespace ayt::video
