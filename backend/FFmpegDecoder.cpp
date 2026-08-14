#include "FFmpegDecoder.h"

// design.md §2.1 G-01: ffmpeg headers live only in backend .cpp files.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <aytime/Duration.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ayt::video
{

namespace
{

std::string avErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    if (av_strerror(err, buf, sizeof(buf)) < 0)
    {
        std::snprintf(buf, sizeof(buf), "av error %d", err);
    }
    return buf;
}

constexpr AVRational kUsTimeBase{1, 1'000'000};
constexpr int kTargetSampleRate = 48000;
constexpr int kTargetChannels = 2;

VideoPixelFormat mapPixelFormat(AVPixelFormat fmt)
{
    switch (fmt)
    {
    case AV_PIX_FMT_YUV420P: return VideoPixelFormat::I420;
    case AV_PIX_FMT_NV12:    return VideoPixelFormat::NV12;
    case AV_PIX_FMT_RGBA:    return VideoPixelFormat::RGBA8;
    case AV_PIX_FMT_BGRA:    return VideoPixelFormat::BGRA8;
    default:                 return VideoPixelFormat::Unknown;
    }
}

} // namespace

struct FFmpegDecoder::Impl
{
    AVCodecContext* videoCtx = nullptr;
    AVCodecContext* audioCtx = nullptr;
    AVFrame* videoFrame = nullptr;
    AVFrame* audioFrame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* swr = nullptr;
    bool open = false;
    bool decodeAudio = false;

    bool flushed = false;
    bool fedAny = false;
    bool drainDone = false;

    bool audioFlushed = false;
    bool audioFedAny = false;
    bool audioDrainDone = false;

    std::string errorString;
    DecoderOpenParams params;
    std::vector<uint8_t> packed;
    std::vector<float> pcm;
};

FFmpegDecoder::FFmpegDecoder()
    : _impl(std::make_unique<Impl>())
{
}

FFmpegDecoder::~FFmpegDecoder()
{
    close();
}

VideoResult FFmpegDecoder::open(const DecoderOpenParams& params)
{
    if (_impl->open)
    {
        return VideoResult::InvalidState;
    }
    if (params.codecName.empty())
    {
        return VideoResult::InvalidArgument;
    }

    const AVCodec* codec = avcodec_find_decoder_by_name(params.codecName.c_str());
    if (!codec)
    {
        _impl->errorString = "unknown codec: " + params.codecName;
        return VideoResult::UnsupportedFormat;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        return VideoResult::OutOfMemory;
    }
    _impl->videoCtx = ctx;
    _impl->params = params;
    _impl->decodeAudio = params.decodeAudio && params.media.hasAudio
                         && !params.media.audioCodec.empty();

    {
        AVCodecParameters* par = avcodec_parameters_alloc();
        if (!par)
        {
            close();
            return VideoResult::OutOfMemory;
        }
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->codec_id = codec->id;
        par->width = params.media.width;
        par->height = params.media.height;
        par->format = AV_PIX_FMT_YUV420P;
        if (!params.media.videoExtradata.empty())
        {
            const int size = static_cast<int>(params.media.videoExtradata.size());
            par->extradata = static_cast<uint8_t*>(
                av_malloc(static_cast<size_t>(size) + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!par->extradata)
            {
                avcodec_parameters_free(&par);
                close();
                return VideoResult::OutOfMemory;
            }
            std::memcpy(par->extradata, params.media.videoExtradata.data(),
                        static_cast<size_t>(size));
            std::memset(par->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            par->extradata_size = size;
        }
        if (avcodec_parameters_to_context(ctx, par) < 0)
        {
            avcodec_parameters_free(&par);
            _impl->errorString = "avcodec_parameters_to_context failed";
            close();
            return VideoResult::DecodeError;
        }
        avcodec_parameters_free(&par);
    }
    ctx->thread_count = 1;
    ctx->time_base = kUsTimeBase;

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        _impl->errorString = "avcodec_open2 failed";
        close();
        return VideoResult::DecodeError;
    }
    ctx->time_base = kUsTimeBase;

    if (_impl->decodeAudio)
    {
        const AVCodec* aCodec =
            avcodec_find_decoder_by_name(params.media.audioCodec.c_str());
        if (!aCodec)
        {
            _impl->errorString = "unknown audio codec: " + params.media.audioCodec;
            close();
            return VideoResult::UnsupportedFormat;
        }
        AVCodecContext* actx = avcodec_alloc_context3(aCodec);
        if (!actx)
        {
            close();
            return VideoResult::OutOfMemory;
        }
        _impl->audioCtx = actx;

        AVCodecParameters* apar = avcodec_parameters_alloc();
        if (!apar)
        {
            close();
            return VideoResult::OutOfMemory;
        }
        apar->codec_type = AVMEDIA_TYPE_AUDIO;
        apar->codec_id = aCodec->id;
        apar->sample_rate = params.media.audioSampleRate > 0
                                ? params.media.audioSampleRate
                                : kTargetSampleRate;
        const int ch = params.media.audioChannels > 0
                           ? params.media.audioChannels
                           : 1;
        av_channel_layout_default(&apar->ch_layout, ch);
        if (!params.media.audioExtradata.empty())
        {
            const int size = static_cast<int>(params.media.audioExtradata.size());
            apar->extradata = static_cast<uint8_t*>(
                av_malloc(static_cast<size_t>(size) + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!apar->extradata)
            {
                avcodec_parameters_free(&apar);
                close();
                return VideoResult::OutOfMemory;
            }
            std::memcpy(apar->extradata, params.media.audioExtradata.data(),
                        static_cast<size_t>(size));
            std::memset(apar->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            apar->extradata_size = size;
        }
        if (avcodec_parameters_to_context(actx, apar) < 0)
        {
            avcodec_parameters_free(&apar);
            _impl->errorString = "audio avcodec_parameters_to_context failed";
            close();
            return VideoResult::DecodeError;
        }
        avcodec_parameters_free(&apar);
        actx->thread_count = 1;
        actx->time_base = kUsTimeBase;
        if (avcodec_open2(actx, aCodec, nullptr) < 0)
        {
            _impl->errorString = "audio avcodec_open2 failed";
            close();
            return VideoResult::DecodeError;
        }
        actx->time_base = kUsTimeBase;
    }

    _impl->videoFrame = av_frame_alloc();
    _impl->packet = av_packet_alloc();
    if (!_impl->videoFrame || !_impl->packet)
    {
        close();
        return VideoResult::OutOfMemory;
    }
    if (_impl->decodeAudio)
    {
        _impl->audioFrame = av_frame_alloc();
        if (!_impl->audioFrame)
        {
            close();
            return VideoResult::OutOfMemory;
        }
    }

    _impl->open = true;
    _impl->flushed = false;
    _impl->fedAny = false;
    _impl->drainDone = false;
    _impl->audioFlushed = false;
    _impl->audioFedAny = false;
    _impl->audioDrainDone = false;
    return VideoResult::Ok;
}

void FFmpegDecoder::close() noexcept
{
    if (_impl->swr)
    {
        swr_free(&_impl->swr);
    }
    if (_impl->packet)
    {
        av_packet_free(&_impl->packet);
    }
    if (_impl->videoFrame)
    {
        av_frame_free(&_impl->videoFrame);
    }
    if (_impl->audioFrame)
    {
        av_frame_free(&_impl->audioFrame);
    }
    if (_impl->videoCtx)
    {
        avcodec_free_context(&_impl->videoCtx);
    }
    if (_impl->audioCtx)
    {
        avcodec_free_context(&_impl->audioCtx);
    }
    _impl->packed.clear();
    _impl->packed.shrink_to_fit();
    _impl->pcm.clear();
    _impl->pcm.shrink_to_fit();
    _impl->open = false;
    _impl->decodeAudio = false;
}

bool FFmpegDecoder::isOpen() const noexcept
{
    return _impl->open;
}

VideoResult FFmpegDecoder::feedPacket(const VideoPacket& packet)
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }

    auto sendTo = [&](AVCodecContext* ctx, bool isVideo) -> VideoResult {
        if (!ctx)
        {
            return VideoResult::Ok;
        }
        av_packet_unref(_impl->packet);
        if (packet.data)
        {
            if (isVideo)
            {
                _impl->flushed = false;
                _impl->drainDone = false;
            }
            else
            {
                _impl->audioFlushed = false;
                _impl->audioDrainDone = false;
            }
            if (av_new_packet(_impl->packet, static_cast<int>(packet.size)) < 0)
            {
                return VideoResult::OutOfMemory;
            }
            std::memcpy(_impl->packet->data, packet.data, packet.size);
            _impl->packet->pts = av_rescale_q(
                packet.pts.toUs(), kUsTimeBase, ctx->time_base);
            const int err = avcodec_send_packet(ctx, _impl->packet);
            av_packet_unref(_impl->packet);
            if (err == AVERROR(EAGAIN))
            {
                return VideoResult::QueueFull;
            }
            if (err < 0)
            {
                _impl->errorString = avErrorString(err);
                return VideoResult::DecodeError;
            }
            if (isVideo)
            {
                _impl->fedAny = true;
            }
            else
            {
                _impl->audioFedAny = true;
            }
            return VideoResult::Ok;
        }
        // Null packet -> drain.
        if (avcodec_send_packet(ctx, nullptr) < 0)
        {
            return VideoResult::DecodeError;
        }
        return VideoResult::Ok;
    };

    if (packet.isVideo)
    {
        return sendTo(_impl->videoCtx, true);
    }
    if (_impl->decodeAudio && _impl->audioCtx)
    {
        return sendTo(_impl->audioCtx, false);
    }
    // V1 video-only: drop audio packets.
    return VideoResult::Ok;
}

VideoResult FFmpegDecoder::dequeueFrame(VideoFrame& outFrame)
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }

    outFrame = VideoFrame{};
    if (!_impl->fedAny)
    {
        return VideoResult::Ok;
    }

    av_frame_unref(_impl->videoFrame);
    const int err = avcodec_receive_frame(_impl->videoCtx, _impl->videoFrame);
    if (err == AVERROR(EAGAIN))
    {
        if (_impl->flushed && _impl->drainDone)
        {
            return VideoResult::EndOfStream;
        }
        return VideoResult::Ok;
    }
    if (err == AVERROR_EOF)
    {
        return VideoResult::EndOfStream;
    }
    if (err < 0)
    {
        _impl->errorString = avErrorString(err);
        return VideoResult::DecodeError;
    }

    const auto* f = _impl->videoFrame;
    outFrame.width = f->width;
    outFrame.height = f->height;
    outFrame.format = mapPixelFormat(static_cast<AVPixelFormat>(f->format));
    outFrame.pts = ayt::time::Duration::fromUs(av_rescale_q(
        f->pts, _impl->videoCtx->time_base, kUsTimeBase));
    outFrame.planeOffset[0] = outFrame.planeOffset[1] =
        outFrame.planeOffset[2] = 0;

    auto copyPlaneRows = [](uint8_t* dst, const uint8_t* src,
                            int srcStride, int rows, int rowBytes) {
        for (int y = 0; y < rows; ++y)
        {
            std::memcpy(dst, src + static_cast<std::ptrdiff_t>(y) * srcStride,
                        static_cast<size_t>(rowBytes));
            dst += rowBytes;
        }
        return dst;
    };

    if (outFrame.format == VideoPixelFormat::I420 && f->data[0] && f->data[1]
        && f->data[2] && f->width > 0 && f->height > 0)
    {
        const int w = f->width;
        const int h = f->height;
        const int cw = (w + 1) / 2;
        const int ch = (h + 1) / 2;
        const size_t yBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
        const size_t uBytes = static_cast<size_t>(cw) * static_cast<size_t>(ch);
        _impl->packed.resize(yBytes + uBytes + uBytes);
        uint8_t* dst = _impl->packed.data();
        dst = copyPlaneRows(dst, f->data[0], f->linesize[0], h, w);
        outFrame.planeOffset[1] = static_cast<uint32_t>(dst - _impl->packed.data());
        dst = copyPlaneRows(dst, f->data[1], f->linesize[1], ch, cw);
        outFrame.planeOffset[2] = static_cast<uint32_t>(dst - _impl->packed.data());
        dst = copyPlaneRows(dst, f->data[2], f->linesize[2], ch, cw);
        (void)dst;
        outFrame.stride = static_cast<uint32_t>(w);
        outFrame.dataSize = static_cast<uint32_t>(_impl->packed.size());
        outFrame.data = _impl->packed.data();
    }
    else if (outFrame.format == VideoPixelFormat::NV12 && f->data[0]
             && f->data[1] && f->width > 0 && f->height > 0)
    {
        const int w = f->width;
        const int h = f->height;
        const int ch = (h + 1) / 2;
        const size_t yBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
        const size_t uvBytes = static_cast<size_t>(w) * static_cast<size_t>(ch);
        _impl->packed.resize(yBytes + uvBytes);
        uint8_t* dst = _impl->packed.data();
        dst = copyPlaneRows(dst, f->data[0], f->linesize[0], h, w);
        outFrame.planeOffset[1] = static_cast<uint32_t>(dst - _impl->packed.data());
        dst = copyPlaneRows(dst, f->data[1], f->linesize[1], ch, w);
        (void)dst;
        outFrame.stride = static_cast<uint32_t>(w);
        outFrame.dataSize = static_cast<uint32_t>(_impl->packed.size());
        outFrame.data = _impl->packed.data();
    }
    else if (f->data[0] && f->width > 0 && f->height > 0)
    {
        const int w = f->width;
        const int h = f->height;
        const int bpp =
            (outFrame.format == VideoPixelFormat::RGBA8
             || outFrame.format == VideoPixelFormat::BGRA8)
                ? 4
                : 1;
        const int rowBytes = w * bpp;
        _impl->packed.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(h));
        copyPlaneRows(_impl->packed.data(), f->data[0], f->linesize[0], h,
                      rowBytes);
        outFrame.stride = static_cast<uint32_t>(rowBytes);
        outFrame.dataSize = static_cast<uint32_t>(_impl->packed.size());
        outFrame.data = _impl->packed.data();
    }
    else
    {
        return VideoResult::DecodeError;
    }
    return VideoResult::Ok;
}

VideoResult FFmpegDecoder::dequeueAudioFrame(AudioPcmFrame& outFrame)
{
    outFrame = AudioPcmFrame{};
    if (!_impl->open || !_impl->decodeAudio || !_impl->audioCtx)
    {
        return VideoResult::Ok;
    }
    if (!_impl->audioFedAny)
    {
        return VideoResult::Ok;
    }

    av_frame_unref(_impl->audioFrame);
    const int err = avcodec_receive_frame(_impl->audioCtx, _impl->audioFrame);
    if (err == AVERROR(EAGAIN))
    {
        if (_impl->audioFlushed && _impl->audioDrainDone)
        {
            return VideoResult::EndOfStream;
        }
        return VideoResult::Ok;
    }
    if (err == AVERROR_EOF)
    {
        return VideoResult::EndOfStream;
    }
    if (err < 0)
    {
        _impl->errorString = avErrorString(err);
        return VideoResult::DecodeError;
    }

    AVFrame* f = _impl->audioFrame;
    // (Re)configure swr when input layout changes.
    AVChannelLayout outLayout{};
    av_channel_layout_default(&outLayout, kTargetChannels);
    if (!_impl->swr)
    {
        if (swr_alloc_set_opts2(&_impl->swr,
                               &outLayout, AV_SAMPLE_FMT_FLT, kTargetSampleRate,
                               &f->ch_layout,
                               static_cast<AVSampleFormat>(f->format),
                               f->sample_rate,
                               0, nullptr) < 0
            || swr_init(_impl->swr) < 0)
        {
            av_channel_layout_uninit(&outLayout);
            _impl->errorString = "swr_init failed";
            return VideoResult::DecodeError;
        }
    }
    av_channel_layout_uninit(&outLayout);

    const int outSamples = swr_get_out_samples(_impl->swr, f->nb_samples);
    if (outSamples <= 0)
    {
        return VideoResult::Ok;
    }
    _impl->pcm.resize(static_cast<size_t>(outSamples) * kTargetChannels);
    uint8_t* outPlanes[1] = {
        reinterpret_cast<uint8_t*>(_impl->pcm.data())};
    const int converted = swr_convert(_impl->swr, outPlanes, outSamples,
                                      const_cast<const uint8_t**>(f->extended_data),
                                      f->nb_samples);
    if (converted < 0)
    {
        _impl->errorString = avErrorString(converted);
        return VideoResult::DecodeError;
    }
    if (converted == 0)
    {
        return VideoResult::Ok;
    }

    outFrame.data = _impl->pcm.data();
    outFrame.frameCount = static_cast<uint32_t>(converted);
    outFrame.channels = static_cast<uint16_t>(kTargetChannels);
    outFrame.sampleRate = static_cast<uint32_t>(kTargetSampleRate);
    outFrame.pts = ayt::time::Duration::fromUs(av_rescale_q(
        f->pts, _impl->audioCtx->time_base, kUsTimeBase));
    return VideoResult::Ok;
}

VideoResult FFmpegDecoder::flush()
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }
    if (_impl->videoCtx)
    {
        avcodec_flush_buffers(_impl->videoCtx);
    }
    if (_impl->audioCtx)
    {
        avcodec_flush_buffers(_impl->audioCtx);
    }
    if (_impl->swr)
    {
        swr_free(&_impl->swr);
    }
    _impl->flushed = false;
    _impl->drainDone = false;
    _impl->fedAny = false;
    _impl->audioFlushed = false;
    _impl->audioDrainDone = false;
    _impl->audioFedAny = false;
    return VideoResult::Ok;
}

const char* FFmpegDecoder::lastErrorString() const noexcept
{
    return _impl->errorString.c_str();
}

} // namespace ayt::video
