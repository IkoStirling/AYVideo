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

#include <atomic>
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
    // V5: interrupt_callback abort flag (stop/seek cancel).
    std::atomic<int> interrupt{0};
    // Seek verification peeked a packet — return it from the next read.
    bool pendingPacket = false;
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

    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx)
    {
        return VideoResult::OutOfMemory;
    }

    // V5: abort open/read when interrupt flag is set (stop/seek cancel).
    ctx->interrupt_callback.callback = [](void* opaque) -> int {
        auto* self = static_cast<Impl*>(opaque);
        return self && self->interrupt.load(std::memory_order_relaxed) ? 1 : 0;
    };
    ctx->interrupt_callback.opaque = _impl.get();
    _impl->interrupt.store(false, std::memory_order_relaxed);

    AVDictionary* opts = nullptr;
    if (isHttpUrl(params.path))
    {
        // Timeouts are microseconds for FFmpeg rw_timeout / timeout.
        const int64_t openUs =
            static_cast<int64_t>(params.openTimeoutMs > 0 ? params.openTimeoutMs
                                                         : 10000)
            * 1000;
        const int64_t rwUs =
            static_cast<int64_t>(params.rwTimeoutMs > 0 ? params.rwTimeoutMs
                                                       : 15000)
            * 1000;
        av_dict_set_int(&opts, "timeout", openUs, 0);
        av_dict_set_int(&opts, "rw_timeout", rwUs, 0);
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
        if (params.reconnectDelayMs > 0)
        {
            av_dict_set_int(&opts, "reconnect_delay_max",
                            static_cast<int64_t>(params.reconnectDelayMs), 0);
        }
        // Progressive: keep probe modest so TTFB stays reasonable.
        av_dict_set_int(&opts, "probesize", 2 * 1024 * 1024, 0);
        av_dict_set_int(&opts, "analyzeduration", 2 * 1000 * 1000, 0);
        // Do NOT set seekable=0 here — MP4 often needs a seek to the
        // trailing moov during find_stream_info.
    }

    // avformat_open_input takes ownership of ctx even on failure.
    const int err =
        avformat_open_input(&ctx, params.path.c_str(), nullptr, &opts);
    av_dict_free(&opts);
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

    // Only force AVIO non-seekable when the caller asked for it (e.g. true
    // live / non-Range sources). HTTP progressive with Range stays seekable.
    if (!params.seekable && ctx->pb)
    {
        ctx->pb->seekable = 0;
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
    _impl->lastVideoPtsUs = -1;
    return VideoResult::Ok;
}

void FFmpegDemuxer::close() noexcept
{
    _impl->interrupt.store(1, std::memory_order_relaxed);
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
    _impl->pendingPacket = false;
    _impl->open = false;
    _impl->interrupt.store(0, std::memory_order_relaxed);
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
        if (!_impl->pendingPacket)
        {
            const int err = av_read_frame(_impl->formatContext, _impl->packet);
            if (err < 0)
            {
                _impl->errorString = avErrorString(err);
                return mapAvError(err); // EndOfStream at EOF (design.md §5.4)
            }
        }
        _impl->pendingPacket = false;

        const int streamIndex = _impl->packet->stream_index;
        if (streamIndex != _impl->videoStreamIndex && streamIndex != _impl->audioStreamIndex)
        {
            av_packet_unref(_impl->packet);
            continue;
        }

        const bool isVideo = streamIndex == _impl->videoStreamIndex;
        const AVRational& tb = isVideo ? _impl->videoTimebase : _impl->audioTimebase;
        const int64_t rawPts =
            _impl->packet->pts != AV_NOPTS_VALUE
                ? _impl->packet->pts
                : (_impl->packet->dts != AV_NOPTS_VALUE ? _impl->packet->dts : 0);
        const int64_t rawDts =
            _impl->packet->dts != AV_NOPTS_VALUE
                ? _impl->packet->dts
                : rawPts;

        outPacket = VideoPacket{};
        outPacket.data = _impl->packet->data;
        outPacket.size = static_cast<uint32_t>(_impl->packet->size);
        outPacket.isVideo = isVideo;
        outPacket.streamIndex = streamIndex;
        outPacket.pts = ayt::time::Duration::fromUs(avRescaleUs(rawPts, tb));
        outPacket.dts = ayt::time::Duration::fromUs(avRescaleUs(rawDts, tb));
        if (isVideo)
        {
            _impl->lastVideoPtsUs = outPacket.pts.toUs();
        }
        return VideoResult::Ok;
    }
}

VideoResult FFmpegDemuxer::seek(const ayt::time::Duration& target,
                                bool keyframeOnly)
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }
    if (!_impl->params.seekable)
    {
        return VideoResult::UnsupportedFormat; // design.md §7.3
    }
    if (_impl->formatContext && _impl->formatContext->pb
        && _impl->formatContext->pb->seekable == 0)
    {
        return VideoResult::UnsupportedFormat;
    }

    _impl->interrupt.store(0, std::memory_order_relaxed);
    _impl->pendingPacket = false;
    if (_impl->packet)
    {
        av_packet_unref(_impl->packet);
    }

    std::int64_t targetUs = target.toUs();
    if (targetUs < 0)
    {
        targetUs = 0;
    }
    if (_impl->formatContext->duration > 0
        && targetUs > _impl->formatContext->duration)
    {
        targetUs = _impl->formatContext->duration;
    }

    AVFormatContext* ctx = _impl->formatContext;
    const int vIdx = _impl->videoStreamIndex;
    const AVRational usTb{1, AV_TIME_BASE};
    const int64_t streamTs =
        (vIdx >= 0 && _impl->videoTimebase.num != 0 && _impl->videoTimebase.den != 0)
            ? av_rescale_q(targetUs, usTb, _impl->videoTimebase)
            : targetUs;

    // Peek first video packet after a candidate seek. Long-GOP files often
    // have the previous keyframe far before target (even at t=0) — that is
    // a valid Keyframe scrub land, NOT a failed seek. Never fall through to
    // AVSEEK_FLAG_ANY for keyframe seeks (non-KF land breaks H.264 refs).
    auto runSeek = [&](const char* name, int err) -> bool {
        if (err < 0)
        {
            return false;
        }
        avformat_flush(ctx);

        AVPacket* peek = av_packet_alloc();
        if (!peek)
        {
            _impl->lastVideoPtsUs = -1;
            return true;
        }
        std::int64_t landedUs = -1;
        bool isKey = false;
        for (int i = 0; i < 48; ++i)
        {
            const int rr = av_read_frame(ctx, peek);
            if (rr < 0)
            {
                break;
            }
            if (vIdx >= 0 && peek->stream_index != vIdx)
            {
                av_packet_unref(peek);
                continue;
            }
            const int64_t raw =
                peek->pts != AV_NOPTS_VALUE
                    ? peek->pts
                    : (peek->dts != AV_NOPTS_VALUE ? peek->dts : 0);
            landedUs = avRescaleUs(raw, _impl->videoTimebase);
            isKey = (peek->flags & AV_PKT_FLAG_KEY) != 0;
            av_packet_unref(_impl->packet);
            av_packet_ref(_impl->packet, peek);
            _impl->pendingPacket = true;
            av_packet_unref(peek);
            break;
        }
        av_packet_free(&peek);

        std::fprintf(stderr,
                     "[AYVideo][demux] seek strategy=%s kfOnly=%d target=%.3fs "
                     "landed=%.3fs key=%d streamTs=%lld\n",
                     name, keyframeOnly ? 1 : 0, targetUs / 1e6,
                     landedUs >= 0 ? landedUs / 1e6 : -1.0, isKey ? 1 : 0,
                     static_cast<long long>(streamTs));
        std::fflush(stderr);

        // Accurate path wants to be *near* target. Reject a bogus land at
        // t≈0 only when we asked for mid-file AND this strategy used ANY
        // (handled by not calling ANY below for keyframeOnly).
        _impl->lastVideoPtsUs = -1;
        return true;
    };

    // Keyframe scrub / Accurate pre-roll: always keyframe-at-or-before.
    if (keyframeOnly)
    {
        if (vIdx >= 0
            && runSeek("seek_frame/video/BACKWARD",
                       av_seek_frame(ctx, vIdx, streamTs, AVSEEK_FLAG_BACKWARD)))
        {
            return VideoResult::Ok;
        }
        if (runSeek("seek_frame/default/BACKWARD",
                    av_seek_frame(ctx, -1, targetUs, AVSEEK_FLAG_BACKWARD)))
        {
            return VideoResult::Ok;
        }
        if (vIdx >= 0)
        {
            const int64_t minTs =
                streamTs > 0 ? streamTs - av_rescale_q(30'000'000, usTb,
                                                       _impl->videoTimebase)
                             : 0;
            if (runSeek("seek_file/video/BACKWARD",
                        avformat_seek_file(ctx, vIdx, minTs < 0 ? 0 : minTs,
                                          streamTs, streamTs,
                                          AVSEEK_FLAG_BACKWARD)))
            {
                return VideoResult::Ok;
            }
        }
        _impl->errorString = "keyframe seek failed";
        return VideoResult::DemuxError;
    }

    // Non-keyframe positioning (unused by current player paths).
    if (vIdx >= 0
        && runSeek("seek_frame/video/ANY",
                   av_seek_frame(ctx, vIdx, streamTs, AVSEEK_FLAG_ANY)))
    {
        return VideoResult::Ok;
    }
    if (runSeek("seek_file/default",
                avformat_seek_file(ctx, -1, 0, targetUs, targetUs + 2'000'000, 0)))
    {
        return VideoResult::Ok;
    }
    _impl->errorString = "av_seek_frame failed";
    return VideoResult::DemuxError;
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

VideoResult FFmpegDemuxer::reconnect()
{
    if (_impl->params.path.empty())
    {
        return VideoResult::NotInitialized;
    }
    const DemuxerOpenParams params = _impl->params;
    close();
    return open(params);
}

void FFmpegDemuxer::requestAbort() noexcept
{
    _impl->interrupt.store(1, std::memory_order_relaxed);
}

void FFmpegDemuxer::clearAbort() noexcept
{
    _impl->interrupt.store(0, std::memory_order_relaxed);
}

const char* FFmpegDemuxer::lastErrorString() const noexcept
{
    return _impl->errorString.c_str();
}

} // namespace ayt::video
