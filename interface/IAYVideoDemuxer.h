#pragma once
// IAYVideoDemuxer.h — container demux seam.
//
// design.md §7: separates container parsing (MP4/MKV/WebM/...) from the
// codec pipeline. A demuxer owns its packet buffer — packet pointers
// returned by readNextPacket are valid until the next call on the same
// demuxer.
//
// Thread contract (design.md §14): a demuxer instance is confined to a
// single thread (the demux thread). Multiple instances may run
// concurrently, but a single instance must never be driven from two
// threads.

#include <AYVideoFrame.h>
#include <AYVideoMediaInfo.h>
#include <AYVideoTypes.h>
#include <aytime/Duration.h>

#include <string>

namespace ayt::video
{

struct DemuxerOpenParams
{
    std::string path;          // container file path (V1: file only;
                               // network URLs are a V5 foresight)
    bool seekable = true;      // hint; demuxer may degrade if false
};

class IAYVideoDemuxer
{
public:
    virtual ~IAYVideoDemuxer() = default;

    // Opens the container and parses stream headers. On success the
    // demuxer is positioned at the first packet. Calling open() on an
    // already-open demuxer returns InvalidState (call close() first).
    virtual VideoResult open(const DemuxerOpenParams& params) = 0;

    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;

    // Container metadata. Must be called after a successful open().
    virtual VideoResult getMediaInfo(MediaInfo& outInfo) const = 0;

    // Reads the next packet. Returns Ok with outPacket filled, or
    // EndOfStream when the container is exhausted (no more packets).
    // Never returns a null packet with Ok.
    virtual VideoResult readNextPacket(VideoPacket& outPacket) = 0;

    // Seeks to an absolute presentation time. Invalidates any packet
    // previously returned. Returns InvalidState when !isOpen(),
    // UnsupportedFormat when the container is not seekable.
    virtual VideoResult seek(const ayt::time::Duration& target) = 0;

    // V4 N-10: remap which streams readNextPacket emits. Pass container
    // stream indices (-1 disables that kind). Default = no-op Ok so
    // Null/legacy backends stay valid. Applied by the player on the
    // next play/seek (not mid-decode without flush).
    virtual VideoResult setActiveStreamIndices(int32_t /*videoStreamIndex*/,
                                               int32_t /*audioStreamIndex*/)
    {
        return VideoResult::Ok;
    }
};

} // namespace ayt::video
