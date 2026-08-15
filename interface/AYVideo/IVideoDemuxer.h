#pragma once
// AYVideo/IVideoDemuxer.h — container demux seam.
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

#include <AYVideo/VideoFrame.h>
#include <AYVideo/VideoMediaInfo.h>
#include <AYVideo/VideoTypes.h>
#include <AYTime/Duration.h>

#include <string>

namespace ayt::video
{

struct DemuxerOpenParams
{
    // Container file path OR network URL (V5: http(s) / rtsp / HLS m3u8).
    std::string path;
    // Hint; demuxer may degrade if false. Player forces false for live RTSP.
    bool seekable = true;

    // V5 network options (ignored for local files). Timeouts in ms.
    int32_t openTimeoutMs = 10000;
    int32_t rwTimeoutMs = 15000;
    uint32_t reconnectMax = 0;       // 0 = no DecodeLoop reconnect (local)
    uint32_t reconnectDelayMs = 500; // backoff between reconnect attempts

    // HLS ABR: preferred ceiling bitrate for master-playlist variant pick
    // (FFmpeg `bandwidth`). 0 = demuxer default / auto.
    uint32_t preferredBandwidthBps = 0;
    // RTSP: prefer TCP interleaved transport (more firewall-friendly).
    bool rtspPreferTcp = true;
};

// True when path looks like an HTTP(S) URL (V5 progressive / HLS).
inline bool isHttpUrl(const std::string& path) noexcept
{
    return path.compare(0, 7, "http://") == 0
        || path.compare(0, 8, "https://") == 0;
}

inline bool isRtspUrl(const std::string& path) noexcept
{
    return path.compare(0, 7, "rtsp://") == 0
        || path.compare(0, 8, "rtsps://") == 0;
}

// HTTP(S) URL whose path (before ?/#) ends with .m3u8 — HLS playlist.
inline bool isHlsUrl(const std::string& path) noexcept
{
    if (!isHttpUrl(path))
    {
        return false;
    }
    std::string::size_type end = path.find_first_of("?#");
    if (end == std::string::npos)
    {
        end = path.size();
    }
    if (end < 5)
    {
        return false;
    }
    const char* ext = path.c_str() + (end - 5);
    return ext[0] == '.'
        && (ext[1] == 'm' || ext[1] == 'M')
        && (ext[2] == '3')
        && (ext[3] == 'u' || ext[3] == 'U')
        && (ext[4] == '8');
}

inline bool isNetworkUrl(const std::string& path) noexcept
{
    return isHttpUrl(path) || isRtspUrl(path);
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
    // `keyframeOnly`: land on a keyframe at/before target (scrub / Accurate
    // pre-roll). When false, may use non-keyframe positioning (rare).
    virtual VideoResult seek(const ayt::time::Duration& target,
                             bool keyframeOnly = true) = 0;

    // V4 N-10: remap which streams readNextPacket emits. Pass container
    // stream indices (-1 disables that kind). Default = no-op Ok so
    // Null/legacy backends stay valid. Applied by the player on the
    // next play/seek (not mid-decode without flush).
    virtual VideoResult setActiveStreamIndices(int32_t /*videoStreamIndex*/,
                                               int32_t /*audioStreamIndex*/)
    {
        return VideoResult::Ok;
    }

    // Soft-subtitle stream for readNextPacket emission (-1 = off). Default
    // no-op so Null/legacy backends stay valid.
    virtual VideoResult setActiveSubtitleStreamIndex(int32_t /*streamIndex*/)
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

    // Clear a prior requestAbort() so seek / a new DecodeLoop can read again.
    virtual void clearAbort() noexcept {}
};

} // namespace ayt::video
