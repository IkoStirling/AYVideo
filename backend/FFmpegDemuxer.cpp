#include "FFmpegDemuxer.h"

// design.md §2.1 G-01: ffmpeg headers live only in backend .cpp files.
// The public surface (include/ + interface/) never sees these.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
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
    // Last video packet pts (µs) for V4 bidirectional seek direction.
    std::int64_t lastVideoPtsUs = -1;
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
    _impl->lastVideoPtsUs = -1;
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
        outInfo.audioSampleRate = par->sample_rate;
        outInfo.audioChannels = par->ch_layout.nb_channels;
        if (par->extradata && par->extradata_size > 0)
        {
            outInfo.audioExtradata.assign(
                par->extradata, par->extradata + par->extradata_size);
        }
    }
    if (ctx->duration != AV_NOPTS_VALUE)
    {
        outInfo.durationSec = static_cast<double>(ctx->duration) /
                              1'000'000.0; // AV_TIME_BASE = 1e6 µs
    }

    // V4: enumerate all video / audio / subtitle streams.
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
    {
        const AVStream* st = ctx->streams[i];
        if (!st || !st->codecpar)
        {
            continue;
        }
        AVDictionaryEntry* lang =
            av_dict_get(st->metadata, "language", nullptr, 0);
        AVDictionaryEntry* title =
            av_dict_get(st->metadata, "title", nullptr, 0);
        const char* codecName = nullptr;
        if (const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id))
        {
            codecName = codec->name;
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            VideoTrackInfo track{};
            track.streamIndex = static_cast<int32_t>(i);
            if (codecName)
            {
                track.codec = codecName;
            }
            if (lang && lang->value)
            {
                track.language = lang->value;
            }
            if (title && title->value)
            {
                track.title = title->value;
            }
            track.width = st->codecpar->width;
            track.height = st->codecpar->height;
            AVRational fps = st->avg_frame_rate;
            if (fps.num <= 0 || fps.den <= 0)
            {
                fps = st->r_frame_rate;
            }
            if (fps.num > 0 && fps.den > 0)
            {
                track.frameRate = static_cast<double>(fps.num) /
                                  static_cast<double>(fps.den);
            }
            outInfo.videoTracks.push_back(track);
            continue;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            AudioTrackInfo track{};
            track.streamIndex = static_cast<int32_t>(i);
            if (codecName)
            {
                track.codec = codecName;
            }
            if (lang && lang->value)
            {
                track.language = lang->value;
            }
            if (title && title->value)
            {
                track.title = title->value;
            }
            track.sampleRate = st->codecpar->sample_rate;
            track.channels = st->codecpar->ch_layout.nb_channels;
            outInfo.audioTracks.push_back(track);
            continue;
        }
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
        {
            continue;
        }
        SubtitleTrackInfo track{};
        track.streamIndex = static_cast<int32_t>(i);
        if (codecName)
        {
            track.codec = codecName;
        }
        switch (st->codecpar->codec_id)
        {
        case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_SSA:
            track.kind = SubtitleKind::Ass;
            break;
        case AV_CODEC_ID_DVD_SUBTITLE:
        case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        case AV_CODEC_ID_DVB_SUBTITLE:
            track.kind = SubtitleKind::Bitmap;
            break;
        case AV_CODEC_ID_SUBRIP:
        case AV_CODEC_ID_TEXT:
        case AV_CODEC_ID_WEBVTT:
        case AV_CODEC_ID_MOV_TEXT:
            track.kind = SubtitleKind::Text;
            break;
        default:
            track.kind = SubtitleKind::Unknown;
            break;
        }
        if (lang && lang->value)
        {
            track.language = lang->value;
        }
        if (title && title->value)
        {
            track.title = title->value;
        }
        outInfo.subtitleTracks.push_back(track);
    }
    outInfo.hasSubtitles = !outInfo.subtitleTracks.empty();
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
        if (isVideo)
        {
            _impl->lastVideoPtsUs = outPacket.pts.toUs();
        }
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

    // V4 bidirectional polish: when seeking at/after the last read
    // video pts, try a non-BACKWARD seek first (less GOP rewind). Fall
    // back to BACKWARD on failure. Player still applies _minPresentPts.
    const std::int64_t targetUs = target.toUs();
    const bool preferForward =
        _impl->lastVideoPtsUs >= 0 && targetUs >= _impl->lastVideoPtsUs;
    int flags = preferForward ? 0 : AVSEEK_FLAG_BACKWARD;
    if (av_seek_frame(_impl->formatContext, -1, targetUs, flags) < 0)
    {
        if (preferForward)
        {
            flags = AVSEEK_FLAG_BACKWARD;
            if (av_seek_frame(_impl->formatContext, -1, targetUs, flags) < 0)
            {
                _impl->errorString = "av_seek_frame failed";
                return VideoResult::DemuxError;
            }
        }
        else
        {
            _impl->errorString = "av_seek_frame failed";
            return VideoResult::DemuxError;
        }
    }
    avformat_flush(_impl->formatContext);
    _impl->lastVideoPtsUs = -1; // unknown until next read
    return VideoResult::Ok;
}

VideoResult FFmpegDemuxer::setActiveStreamIndices(int32_t videoStreamIndex,
                                                   int32_t audioStreamIndex)
{
    if (!_impl->open || !_impl->formatContext)
    {
        return VideoResult::NotInitialized;
    }
    const AVFormatContext* ctx = _impl->formatContext;
    if (videoStreamIndex >= 0)
    {
        if (static_cast<unsigned>(videoStreamIndex) >= ctx->nb_streams
            || !ctx->streams[videoStreamIndex]
            || ctx->streams[videoStreamIndex]->codecpar->codec_type
                   != AVMEDIA_TYPE_VIDEO)
        {
            return VideoResult::InvalidArgument;
        }
        _impl->videoStreamIndex = videoStreamIndex;
        _impl->videoTimebase = ctx->streams[videoStreamIndex]->time_base;
    }
    else
    {
        return VideoResult::InvalidArgument; // video required
    }
    if (audioStreamIndex >= 0)
    {
        if (static_cast<unsigned>(audioStreamIndex) >= ctx->nb_streams
            || !ctx->streams[audioStreamIndex]
            || ctx->streams[audioStreamIndex]->codecpar->codec_type
                   != AVMEDIA_TYPE_AUDIO)
        {
            return VideoResult::InvalidArgument;
        }
        _impl->audioStreamIndex = audioStreamIndex;
        _impl->audioTimebase = ctx->streams[audioStreamIndex]->time_base;
    }
    else
    {
        _impl->audioStreamIndex = -1;
    }
    return VideoResult::Ok;
}

const char* FFmpegDemuxer::lastErrorString() const noexcept
{
    return _impl->errorString.c_str();
}

} // namespace ayt::video
