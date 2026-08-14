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
    // Container file path OR network URL (V5: http:// / https://).
    std::string path;
    // Hint; demuxer may degrade if false. Player forces false for URLs.
    bool seekable = true;

    // V5 network options (ignored for local files). Timeouts in ms.
    int32_t openTimeoutMs = 10000;
    int32_t rwTimeoutMs = 15000;
    uint32_t reconnectMax = 0;       // 0 = no DecodeLoop reconnect (local)
    uint32_t reconnectDelayMs = 500; // backoff between reconnect attempts
};

// True when path looks like an HTTP(S) URL (V5 progressive streaming).
inline bool isHttpUrl(const std::string& path) noexcept
{
    return path.compare(0, 7, "http://") == 0
        || path.compare(0, 8, "https://") == 0;
}

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

    // V5: close + reopen with the last successful open params (network
    // reconnect). Default InvalidState for backends that do not support it.
    virtual VideoResult reconnect()
    {
        return VideoResult::InvalidState;
    }

    // V5: cooperatively abort a blocking open/read (interrupt callback).
    virtual void requestAbort() noexcept {}
};

} // namespace ayt::video
