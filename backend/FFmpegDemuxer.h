#pragma once
// FFmpegDemuxer.h — libavformat container demuxer (V1).
//
// design.md §7: container parsing via avformat_*. Owns its packet
// buffer — VideoPacket pointers returned by readNextPacket are valid
// until the next call on the same demuxer (§4.5 data ownership).
//
// The header is ffmpeg-free (PIMPL): only the .cpp sees <libavformat/...>
// (design.md §2.1 G-01 — guard scans include/ + interface/; backend
// headers stay clean so they can never become public).
//
// Thread contract (design.md §4.4): one instance = one thread (the
// demux thread). Never drive a single instance from two threads.

#include <AYVideoMediaInfo.h>
#include <AYVideoTypes.h>
#include <IAYVideoDemuxer.h>

#include <memory>

namespace ayt::video
{

class FFmpegDemuxer : public IAYVideoDemuxer
{
public:
    FFmpegDemuxer();
    ~FFmpegDemuxer() override;

    FFmpegDemuxer(const FFmpegDemuxer&) = delete;
    FFmpegDemuxer& operator=(const FFmpegDemuxer&) = delete;

    VideoResult open(const DemuxerOpenParams& params) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    VideoResult getMediaInfo(MediaInfo& outInfo) const override;
    VideoResult readNextPacket(VideoPacket& outPacket) override;
    VideoResult seek(const ayt::time::Duration& target) override;
    VideoResult setActiveStreamIndices(int32_t videoStreamIndex,
                                       int32_t audioStreamIndex) override;
    VideoResult reconnect() override;
    void requestAbort() noexcept override;

    // Diagnostics (tests): last av* error string, or "".
    const char* lastErrorString() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::video
