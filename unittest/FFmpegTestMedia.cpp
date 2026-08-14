#include "FFmpegTestMedia.h"

// design.md §2.1 G-01 note: this is a TEST-ONLY TU; the guard scans
// include/ + interface/ only, so libav* includes are legal here.

#include "AYTest.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

#include <cstdio>
#include <cstring>

namespace ayt::testmedia
{

bool generateClip(const std::string& path, bool withAudio,
                  int32_t frames, int32_t width, int32_t height, double fps,
                  std::string& outError)
{
    AVFormatContext* fmt = nullptr;
    const AVCodec* vEnc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!vEnc)
    {
        outError = "no mpeg4 encoder";
        return false;
    }
    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", path.c_str()) < 0 || !fmt)
    {
        outError = "alloc output context failed";
        return false;
    }

    const AVRational tb{1, static_cast<int>(fps)};
    const AVRational fr{static_cast<int>(fps), 1};

    // -- video stream --------------------------------------------------------
    AVCodecContext* vctx = avcodec_alloc_context3(vEnc);
    vctx->width = width;
    vctx->height = height;
    vctx->time_base = tb;
    vctx->framerate = fr;
    vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    vctx->gop_size = 12;
    vctx->max_b_frames = 0;
    vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // mp4 wants extradata in header
    AVStream* vstream = avformat_new_stream(fmt, vEnc);
    if (avcodec_open2(vctx, vEnc, nullptr) < 0)
    {
        outError = "video encoder open failed";
        avcodec_free_context(&vctx);
        avformat_free_context(fmt);
        return false;
    }
    avcodec_parameters_from_context(vstream->codecpar, vctx);
    vstream->time_base = tb;
    vstream->avg_frame_rate = fr;
    vstream->r_frame_rate = fr;

    // -- audio stream (native AAC) ------------------------------------------
    AVCodecContext* actx = nullptr;
    AVStream* astream = nullptr;
    const AVCodec* aEnc = withAudio ? avcodec_find_encoder(AV_CODEC_ID_AAC) : nullptr;
    if (aEnc)
    {
        actx = avcodec_alloc_context3(aEnc);
        actx->sample_rate = 48000;
        av_channel_layout_default(&actx->ch_layout, 1); // mono (FFmpeg 5+)
        actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        actx->bit_rate = 96'000;
        const AVRational audioTb{1, 48000};
        actx->time_base = audioTb;
        actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        astream = avformat_new_stream(fmt, aEnc);
        if (avcodec_open2(actx, aEnc, nullptr) < 0)
        {
            outError = "audio encoder open failed";
            avcodec_free_context(&vctx);
            avformat_free_context(fmt);
            return false;
        }
        avcodec_parameters_from_context(astream->codecpar, actx);
        astream->time_base = audioTb;
    }

    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0)
    {
        outError = "avio_open failed";
        avcodec_free_context(&vctx);
        if (actx) avcodec_free_context(&actx);
        avformat_free_context(fmt);
        return false;
    }
    if (avformat_write_header(fmt, nullptr) < 0)
    {
        outError = "write_header failed";
        avio_closep(&fmt->pb);
        avcodec_free_context(&vctx);
        if (actx) avcodec_free_context(&actx);
        avformat_free_context(fmt);
        return false;
    }

    // -- video frames --------------------------------------------------------
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);

    for (int32_t i = 0; i < frames; ++i)
    {
        frame->pts = i;
        av_frame_make_writable(frame);
        const int gray = 16 + (i * 14) % 220;
        for (int y = 0; y < height; ++y)
        {
            std::memset(frame->data[0] + y * frame->linesize[0], gray, width);
        }
        for (int y = 0; y < height / 2; ++y)
        {
            std::memset(frame->data[1] + y * frame->linesize[1], 128, width / 2);
            std::memset(frame->data[2] + y * frame->linesize[2], 128, width / 2);
        }

        AVPacket* pkt = av_packet_alloc();
        if (avcodec_send_frame(vctx, frame) == 0)
        {
            while (avcodec_receive_packet(vctx, pkt) == 0)
            {
                av_packet_rescale_ts(pkt, vctx->time_base, vstream->time_base);
                pkt->stream_index = vstream->index;
                pkt->duration = 1;
                av_interleaved_write_frame(fmt, pkt);
                av_packet_unref(pkt);
            }
        }
        av_packet_free(&pkt);
    }
    av_frame_free(&frame);

    // flush the video encoder
    AVPacket* pkt = av_packet_alloc();
    avcodec_send_frame(vctx, nullptr);
    while (avcodec_receive_packet(vctx, pkt) == 0)
    {
        av_packet_rescale_ts(pkt, vctx->time_base, vstream->time_base);
        pkt->stream_index = vstream->index;
        pkt->duration = 1;
        av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // -- audio frames (silence) ---------------------------------------------
    if (actx)
    {
        int frameSize = actx->frame_size;
        if (frameSize <= 0)
        {
            frameSize = 1024; // AAC default
        }
        AVFrame* aframe = av_frame_alloc();
        aframe->format = actx->sample_fmt;
        aframe->sample_rate = actx->sample_rate;
        av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
        aframe->nb_samples = frameSize;
        if (av_frame_get_buffer(aframe, 0) < 0)
        {
            outError = "audio frame alloc failed";
            av_frame_free(&aframe);
            avcodec_free_context(&vctx);
            avcodec_free_context(&actx);
            avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            return false;
        }
        const int audioFrames =
            (frames * 48000 / static_cast<int>(fps)) / frameSize;
        AVPacket* apkt = av_packet_alloc();
        for (int i = 0; i < audioFrames; ++i)
        {
            aframe->pts = static_cast<int64_t>(i) * frameSize;
            av_frame_make_writable(aframe);
            // FLTP: zero each planar channel.
            for (int ch = 0; ch < aframe->ch_layout.nb_channels; ++ch)
            {
                if (aframe->data[ch])
                {
                    std::memset(aframe->data[ch], 0,
                                static_cast<size_t>(aframe->nb_samples) * sizeof(float));
                }
            }

            if (avcodec_send_frame(actx, aframe) == 0)
            {
                while (avcodec_receive_packet(actx, apkt) == 0)
                {
                    av_packet_rescale_ts(apkt, actx->time_base, astream->time_base);
                    apkt->stream_index = astream->index;
                    av_interleaved_write_frame(fmt, apkt);
                    av_packet_unref(apkt);
                }
            }
        }
        // Drain the AAC encoder (avoids "frames left in the queue").
        avcodec_send_frame(actx, nullptr);
        while (avcodec_receive_packet(actx, apkt) == 0)
        {
            av_packet_rescale_ts(apkt, actx->time_base, astream->time_base);
            apkt->stream_index = astream->index;
            av_interleaved_write_frame(fmt, apkt);
            av_packet_unref(apkt);
        }
        av_packet_free(&apkt);
        av_frame_free(&aframe);
    }

    av_write_trailer(fmt);
    avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return true;
}

std::string tempClipPath(const char* tag)
{
    char buf[512];
    const std::string tmp = ::ayt::test::testTmpDir().string();
    std::snprintf(buf, sizeof(buf), "%s\\ayvideo_%s.mp4", tmp.c_str(), tag);
    return buf;
}

GeneratedClip makeClip(bool withAudio)
{
    GeneratedClip c;
    c.path = tempClipPath(withAudio ? "av" : "v");
    c.width = kGenWidth;
    c.height = kGenHeight;
    c.videoFrames = kGenFrames;
    c.fps = kGenFps;
    c.hasAudio = withAudio;
    std::string err;
    if (!generateClip(c.path, withAudio, c.videoFrames, c.width, c.height,
                      c.fps, err))
    {
        fprintf(stderr, "[testmedia] generation failed: %s\n", err.c_str());
        CHECK_TRUE(false);
        c.path.clear();
    }
    return c;
}

} // namespace ayt::testmedia
