#include "FFmpegDecoder.h"

// design.md §2.1 G-01: ffmpeg headers live only in backend .cpp files.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
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

// Microsecond time base for pts rescale (named — MSVC C4576).
constexpr AVRational kUsTimeBase{1, 1'000'000};

// Map our pixel format to the public surface (design.md §5.6).
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
    AVCodecContext* codecContext = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;      // scratch packet for sendPacket
    bool open = false;
    bool flushed = false;            // flush() called; EOS after drain
    bool fedAny = false;             // at least one packet was fed
    bool drainDone = false;          // receive loop reported EAGAIN post-flush
    std::string errorString;
    DecoderOpenParams params;
    // Packed contiguous pixels for the last dequeued frame (§4.5 / A-05).
    // AVFrame planes are often non-contiguous (separate allocs with holes
    // between addresses) — FrameQueue must never memcpy a span across
    // those holes (ACCESS_VIOLATION on Windows).
    std::vector<uint8_t> packed;
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
        // Fall back to the codec id carried by the media info path? V1
        // contract: open by name (design.md §8.1); empty -> InvalidArgument.
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
    _impl->codecContext = ctx;
    _impl->params = params;

    // Build codec parameters from MediaInfo (width/height/extradata) and
    // apply via avcodec_parameters_to_context — more reliable than setting
    // ctx fields alone (mpeg4 needs VOL in extradata before get_buffer).
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
    ctx->thread_count = 1; // V1: single-thread decode (§4.4 / A-07)

    // V1 timeline: anchor the codec time base to microseconds so
    // feed/dequeue pts round-trip exactly matches demuxer µs timestamps
    // (avoids CFR frameRate estimate noise — e.g. probe reporting 27 fps
    // for a 25 fps synthetic clip, which otherwise yields 37037 µs steps).
    ctx->time_base = kUsTimeBase;

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        _impl->errorString = "avcodec_open2 failed";
        close();
        return VideoResult::DecodeError;
    }

    // Codecs may overwrite time_base during open — restore µs anchor.
    ctx->time_base = kUsTimeBase;

    _impl->frame = av_frame_alloc();
    _impl->packet = av_packet_alloc();
    if (!_impl->frame || !_impl->packet)
    {
        close();
        return VideoResult::OutOfMemory;
    }

    _impl->open = true;
    _impl->flushed = false;
    _impl->fedAny = false;
    _impl->drainDone = false;
    return VideoResult::Ok;
}

void FFmpegDecoder::close() noexcept
{
    if (_impl->packet)
    {
        av_packet_free(&_impl->packet);
    }
    if (_impl->frame)
    {
        av_frame_free(&_impl->frame);
    }
    if (_impl->codecContext)
    {
        avcodec_free_context(&_impl->codecContext);
    }
    _impl->packed.clear();
    _impl->packed.shrink_to_fit();
    _impl->open = false;
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

    if (!packet.isVideo)
    {
        // V1 pipeline is video-only (design.md §8.1, decodeAudio=false);
        // audio packets are dropped here, never sent to the video codec.
        return VideoResult::Ok;
    }

    av_packet_unref(_impl->packet);
    if (packet.data)
    {
        // Real data resumes the codec after a flush (seek/stop flush
        // sequence, §8.3): leave the drain state so dequeueFrame reports
        // Ok + null ("no frame yet") instead of EndOfStream.
        _impl->flushed = false;
        _impl->drainDone = false;

        av_packet_unref(_impl->packet);
        if (av_new_packet(_impl->packet, static_cast<int>(packet.size)) < 0)
        {
            return VideoResult::OutOfMemory;
        }
        std::memcpy(_impl->packet->data, packet.data, packet.size);
        _impl->packet->pts = av_rescale_q(
            packet.pts.toUs(), kUsTimeBase,
            _impl->codecContext->time_base);

        const int err = avcodec_send_packet(_impl->codecContext, _impl->packet);
        if (err == AVERROR(EAGAIN))
        {
            // Caller must dequeueFrame then retry the same packet
            // (design.md §5.4 QueueFull; DecodeLoop handles the retry).
            av_packet_unref(_impl->packet);
            return VideoResult::QueueFull;
        }
        if (err < 0)
        {
            _impl->errorString = avErrorString(err);
            return VideoResult::DecodeError;
        }
        _impl->fedAny = true;
        return VideoResult::Ok;
    }

    // Null packet -> flush the decoder (ffmpeg convention).
    if (avcodec_send_packet(_impl->codecContext, nullptr) < 0)
    {
        return VideoResult::DecodeError;
    }
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
        // design.md §6.2: Ok + null data = "no frame ready yet".
        return VideoResult::Ok;
    }

    av_frame_unref(_impl->frame);
    const int err = avcodec_receive_frame(_impl->codecContext, _impl->frame);
    if (err == AVERROR(EAGAIN))
    {
        if (_impl->flushed && _impl->drainDone)
        {
            return VideoResult::EndOfStream; // flush drained (design.md §6.2)
        }
        return VideoResult::Ok; // no frame ready yet
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

    // Frame ready. Pack planes into a contiguous buffer owned by this
    // decoder (valid until the next dequeueFrame/flush — §4.5). Never
    // hand AVFrame plane pointers to callers: plane addresses can be
    // non-contiguous with unmapped holes between them; a span memcpy
    // across that hole is ACCESS_VIOLATION (FrameQueue::push).
    const auto* f = _impl->frame;
    outFrame.width = f->width;
    outFrame.height = f->height;
    outFrame.format = mapPixelFormat(
        static_cast<AVPixelFormat>(f->format));
    outFrame.pts = ayt::time::Duration::fromUs(av_rescale_q(
        f->pts, _impl->codecContext->time_base, kUsTimeBase));
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
        const size_t vBytes = uBytes;
        _impl->packed.resize(yBytes + uBytes + vBytes);
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
        // Packed RGB / unknown single-plane: copy luma/plane0 tightly.
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

VideoResult FFmpegDecoder::flush()
{
    if (!_impl->open)
    {
        return VideoResult::NotInitialized;
    }
    // Seek / stop / replay restart (design.md §8.3): reset codec state
    // so subsequent feedPacket calls are accepted. EOS draining uses a
    // null feedPacket (avcodec_send_packet nullptr), not this path —
    // sending nullptr here left the codec in drain mode and made the
    // next play() feed return DecodeError.
    if (_impl->codecContext)
    {
        avcodec_flush_buffers(_impl->codecContext);
    }
    _impl->flushed = false;
    _impl->drainDone = false;
    _impl->fedAny = false;
    return VideoResult::Ok;
}

const char* FFmpegDecoder::lastErrorString() const noexcept
{
    return _impl->errorString.c_str();
}

} // namespace ayt::video
