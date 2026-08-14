#include "FFmpegDemuxer.h"

// design.md §2.1 G-01: ffmpeg headers live only in backend .cpp files.
// The public surface (include/ + interface/) never sees these.
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/time.h>
}

#include <aytime/Duration.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ayt::video
{

namespace
{

// Convert an AVRational timebase to microseconds.
std::int64_t avRescaleUs(std::int64_t value, const AVRational& tb)
{
    if (tb.num == 0 || tb.den == 0)
    {
        return 0;
    }
    // Named AVRational — MSVC rejects C99 compound literals in C++ (C4576).
    const AVRational usTb{1, 1'000'000};
    return av_rescale_q(value, tb, usTb);
}

// av_err2str is a GNU C compound-literal macro; use av_strerror on MSVC.
std::string avErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    if (av_strerror(err, buf, sizeof(buf)) < 0)
    {
        std::snprintf(buf, sizeof(buf), "av error %d", err);
    }
    return buf;
}

// Map an ffmpeg error code to our result-code discipline (design.md §5.4).
VideoResult mapAvError(int avErr)
{
    if (avErr == AVERROR_EOF)
    {
        return VideoResult::EndOfStream;
    }
    if (avErr == AVERROR_INVALIDDATA)
    {
        return VideoResult::UnsupportedFormat;
    }
    return VideoResult::DemuxError;
}

} // namespace

struct FFmpegDemuxer::Impl
{
    AVFormatContext* formatContext = nullptr;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;
    AVRational videoTimebase{};
    AVRational audioTimebase{};
    AVPacket* packet = nullptr;      // persistent packet buffer (owns data)
    bool open = false;
    std::string errorString;         // last av error text (diagnostics)
    DemuxerOpenParams params;
};

FFmpegDemuxer::FFmpegDemuxer()
    : _impl(std::make_unique<Impl>())
{
    // avformat is thread-safe at the library level; per-instance
    // serialization is the caller's duty (design.md §4.4).
}

FFmpegDemuxer::~FFmpegDemuxer()
{
    close();
}

VideoResult FFmpegDemuxer::open(const DemuxerOpenParams& params)
{
    if (_impl->open)
    {
        return VideoResult::InvalidState; // open() on an open demuxer
    }

    AVFormatContext* ctx = nullptr;
    // avformat_open_input takes ownership of ctx even on failure.
    const int err = avformat_open_input(&ctx, params.path.c_str(), nullptr, nullptr);
    if (err < 0)
    {
        _impl->errorString = avErrorString(err);
        if (ctx)
        {
            avformat_close_input(&ctx);
        }
        return VideoResult::DemuxError;
    }
    _impl->formatContext = ctx;
    _impl->params = params;

    if (avformat_find_stream_info(ctx, nullptr) < 0)
    {
        _impl->errorString = "avformat_find_stream_info failed";
        close();
        return VideoResult::DemuxError;
    }

    // Pick the first video stream and the first audio stream (design.md
    // §7.3: no multi-track selection in V1).
    _impl->videoStreamIndex = -1;
    _impl->audioStreamIndex = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
    {
        const AVStream* st = ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && _impl->videoStreamIndex < 0)
        {
            _impl->videoStreamIndex = static_cast<int>(i);
            _impl->videoTimebase = st->time_base;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && _impl->audioStreamIndex < 0)
        {
            _impl->audioStreamIndex = static_cast<int>(i);
            _impl->audioTimebase = st->time_base;
        }
    }
    if (_impl->videoStreamIndex < 0)
    {
        _impl->errorString = "no video stream found";
        close();
        return VideoResult::StreamNotFound; // design.md §5.4 code 9
    }

    _impl->packet = av_packet_alloc();
    if (!_impl->packet)
    {
        close();
        return VideoResult::OutOfMemory;
    }

    _impl->open = true;
    return VideoResult::Ok;
}

void FFmpegDemuxer::close() noexcept
{
    if (_impl->packet)
    {
        av_packet_free(&_impl->packet);
    }
    if (_impl->formatContext)
    {
        avformat_close_input(&_impl->formatContext);
    }
    _impl->videoStreamIndex = -1;
    _impl->audioStreamIndex = -1;
    _impl->open = false;
}

bool FFmpegDemuxer::isOpen() const noexcept
{
    return _impl->open;
}

VideoResult FFmpegDemuxer::getMediaInfo(MediaInfo& outInfo) const
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }
    const AVFormatContext* ctx = _impl->formatContext;

    outInfo = MediaInfo{};
    if (_impl->videoStreamIndex >= 0)
    {
        const AVCodecParameters* par =
            ctx->streams[_impl->videoStreamIndex]->codecpar;
        outInfo.width = par->width;
        outInfo.height = par->height;
        outInfo.hasVideo = true;
        if (const AVCodec* codec = avcodec_find_decoder(par->codec_id))
        {
            outInfo.videoCodec = codec->name;
        }
        const AVStream* st = ctx->streams[_impl->videoStreamIndex];
        const AVRational& tb = st->time_base;
        // Prefer avg_frame_rate; fall back to r_frame_rate, then 1/tb.
        // When the container remaps time_base (mp4), find_stream_info
        // sometimes estimates a bogus avg (e.g. 27 for a 25 fps CFR
        // clip). Prefer an integer fps derived from time_base when
        // tb.num == 1 and tb.den is a plausible fps.
        AVRational fps = st->avg_frame_rate;
        if (fps.num <= 0 || fps.den <= 0)
        {
            fps = st->r_frame_rate;
        }
        if (tb.num == 1 && tb.den >= 1 && tb.den <= 120)
        {
            outInfo.frameRate = static_cast<double>(tb.den);
        }
        else if (fps.num > 0 && fps.den > 0)
        {
            outInfo.frameRate = static_cast<double>(fps.num) /
                                static_cast<double>(fps.den);
        }
        if (par->extradata && par->extradata_size > 0)
        {
            outInfo.videoExtradata.assign(
                par->extradata, par->extradata + par->extradata_size);
        }
    }
    if (_impl->audioStreamIndex >= 0)
    {
        const AVCodecParameters* par =
            ctx->streams[_impl->audioStreamIndex]->codecpar;
        outInfo.hasAudio = true;
        if (const AVCodec* codec = avcodec_find_decoder(par->codec_id))
        {
            outInfo.audioCodec = codec->name;
        }
    }
    if (ctx->duration != AV_NOPTS_VALUE)
    {
        outInfo.durationSec = static_cast<double>(ctx->duration) /
                              1'000'000.0; // AV_TIME_BASE = 1e6 µs
    }
    return VideoResult::Ok;
}

VideoResult FFmpegDemuxer::readNextPacket(VideoPacket& outPacket)
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }
    if (!_impl->packet)
    {
        return VideoResult::InvalidState;
    }

    // Loop to skip unknown streams (subtitles, data, ...).
    for (;;)
    {
        const int err = av_read_frame(_impl->formatContext, _impl->packet);
        if (err < 0)
        {
            _impl->errorString = avErrorString(err);
            return mapAvError(err); // EndOfStream at EOF (design.md §5.4)
        }
        const int streamIndex = _impl->packet->stream_index;
        if (streamIndex != _impl->videoStreamIndex && streamIndex != _impl->audioStreamIndex)
        {
            av_packet_unref(_impl->packet);
            continue;
        }

        const bool isVideo = streamIndex == _impl->videoStreamIndex;
        const AVRational& tb = isVideo ? _impl->videoTimebase : _impl->audioTimebase;

        outPacket = VideoPacket{};
        outPacket.data = _impl->packet->data;
        outPacket.size = static_cast<uint32_t>(_impl->packet->size);
        outPacket.isVideo = isVideo;
        outPacket.streamIndex = streamIndex;
        outPacket.pts = ayt::time::Duration::fromUs(
            avRescaleUs(_impl->packet->pts != AV_NOPTS_VALUE ? _impl->packet->pts : 0, tb));
        outPacket.dts = ayt::time::Duration::fromUs(
            avRescaleUs(_impl->packet->dts != AV_NOPTS_VALUE ? _impl->packet->dts : 0, tb));
        return VideoResult::Ok;
    }
}

VideoResult FFmpegDemuxer::seek(const ayt::time::Duration& target)
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }
    if (!_impl->params.seekable)
    {
        return VideoResult::UnsupportedFormat; // design.md §7.3
    }

    // av_seek_frame to the target time (µs, AV_TIME_BASE). V1 contract:
    // keyframe-level seek; frame-exact positioning lands in V4.
    const std::int64_t targetUs = target.toUs();
    if (av_seek_frame(_impl->formatContext, -1, targetUs, AVSEEK_FLAG_BACKWARD) < 0)
    {
        _impl->errorString = "av_seek_frame failed";
        return VideoResult::DemuxError;
    }
    avformat_flush(_impl->formatContext);
    return VideoResult::Ok;
}

const char* FFmpegDemuxer::lastErrorString() const noexcept
{
    return _impl->errorString.c_str();
}

} // namespace ayt::video
