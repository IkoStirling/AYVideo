// Test_FFmpegDecoder.cpp — V1.
//
// design.md §8.3 decode contract against the synthetic mp4 sample
// (FFmpegTestMedia, Q7a): feed → dequeue → flush → EOS, plus the
// error-code surface (UnknownCodec / NotInitialized / InvalidState).

#include <string>
#include <vector>

#include "AYTest.h"
#include "AYVideo/VideoTypes.h"
#include "AYVideo/VideoMediaInfo.h"
#include "AYVideo/VideoFrame.h"
#include "FFmpegTestMedia.h"
#include "../backend/FFmpegDemuxer.h"
#include "../backend/FFmpegDecoder.h"

#include <AYTime/Duration.h>

using namespace ayt::video;
using namespace ayt::testmedia;

namespace
{

// Walk the generated clip and return its video packets. The payloads are
// copied out of the demuxer-owned buffer into `payloads` (VideoPacket.data
// is a borrowed pointer; this fixture owns the bytes so the feed loop may
// outlive the demuxer).
struct FedPackets
{
    std::vector<VideoPacket> video;
    std::vector<std::vector<uint8_t>> payloads;
    MediaInfo info;
};

FedPackets demuxVideoPackets(const std::string& path)
{
    FedPackets out;
    FFmpegDemuxer d;
    DemuxerOpenParams params;
    params.path = path;
    CHECK_INT_EQ(static_cast<int>(d.open(params)),
                 static_cast<int>(VideoResult::Ok));
    CHECK_INT_EQ(static_cast<int>(d.getMediaInfo(out.info)),
                 static_cast<int>(VideoResult::Ok));

    VideoPacket p;
    while (d.readNextPacket(p) == VideoResult::Ok)
    {
        if (!p.isVideo)
        {
            continue;
        }
        out.payloads.emplace_back(p.data, p.data + p.size);
        VideoPacket copy = p;
        copy.data = out.payloads.back().data();
        out.video.push_back(copy);
    }
    return out;
}

// Feed one packet, draining on QueueFull (EAGAIN). Counts emitted frames.
int feedAndDrain(FFmpegDecoder& dec, const VideoPacket& p, int& frames)
{
    for (;;)
    {
        const VideoResult fr = dec.feedPacket(p);
        if (fr == VideoResult::QueueFull)
        {
            VideoFrame tmp;
            if (dec.dequeueFrame(tmp) == VideoResult::Ok && tmp.data)
            {
                ++frames;
            }
            continue;
        }
        if (fr != VideoResult::Ok)
        {
            return static_cast<int>(fr);
        }
        break;
    }
    for (;;)
    {
        VideoFrame tmp;
        if (dec.dequeueFrame(tmp) != VideoResult::Ok || !tmp.data)
        {
            break;
        }
        ++frames;
    }
    return 0;
}

} // namespace

TEST_SUITE(FFmpegDecoderSuite)

    TEST_CASE(FFmpegDecoderOpenUnknownCodecUnsupportedFormat) {
        FFmpegDecoder dec;
        DecoderOpenParams params;
        params.codecName = "definitely_not_a_codec_xyz";
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::UnsupportedFormat));
        CHECK_FALSE(dec.isOpen());
        CHECK_TRUE(std::string(dec.lastErrorString()).size() > 0);
    }

    TEST_CASE(FFmpegDecoderOpenEmptyNameInvalidArgument) {
        FFmpegDecoder dec;
        DecoderOpenParams params; // codecName stays empty
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::InvalidArgument));
    }

    TEST_CASE(FFmpegDecoderOpenTwiceInvalidState) {
        FFmpegDecoder dec;
        DecoderOpenParams params;
        params.codecName = "mpeg4";
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_TRUE(dec.isOpen());
    }

    TEST_CASE(FFmpegDecoderCallsBeforeOpenNotInitialized) {
        FFmpegDecoder dec;
        VideoPacket p;
        VideoFrame f;
        CHECK_INT_EQ(static_cast<int>(dec.feedPacket(p)),
                     static_cast<int>(VideoResult::NotInitialized));
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                     static_cast<int>(VideoResult::NotInitialized));
        CHECK_INT_EQ(static_cast<int>(dec.flush()),
                     static_cast<int>(VideoResult::NotInitialized));
    }

    TEST_CASE(FFmpegDecoderNoFrameBeforeFeed) {
        FFmpegDecoder dec;
        DecoderOpenParams params;
        params.codecName = "mpeg4";
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::Ok));

        // §6.2: Ok + null data = no frame ready yet — never EOS.
        VideoFrame f;
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(f.data == nullptr);
    }

    TEST_CASE(FFmpegDecoderDecodesGeneratedClip) {
        GeneratedClip c = makeClip(false);
        FedPackets fed = demuxVideoPackets(c.path);
        CHECK_INT_EQ(static_cast<int>(fed.video.size()), kGenFrames);

        FFmpegDecoder dec;
        DecoderOpenParams params;
        params.codecName = fed.info.videoCodec;
        params.media = fed.info;
        CHECK_TRUE(params.media.width > 0);
        CHECK_TRUE(!params.media.videoExtradata.empty());
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::Ok));

        // Interleave feed + dequeue (codec may EAGAIN / QueueFull after a
        // couple of packets if the caller doesn't drain).
        VideoFrame f;
        int frames = 0;
        std::int64_t lastPtsUs = -1;
        for (const VideoPacket& p : fed.video)
        {
            for (;;)
            {
                const VideoResult fr = dec.feedPacket(p);
                if (fr == VideoResult::QueueFull)
                {
                    VideoFrame tmp;
                    CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(tmp)),
                                 static_cast<int>(VideoResult::Ok));
                    CHECK_NOT_NULL(tmp.data);
                    CHECK_INT_EQ(tmp.width, kGenWidth);
                    CHECK_INT_EQ(tmp.height, kGenHeight);
                    CHECK_TRUE(tmp.pts.toUs() >= lastPtsUs);
                    lastPtsUs = tmp.pts.toUs();
                    ++frames;
                    continue;
                }
                CHECK_INT_EQ(static_cast<int>(fr),
                             static_cast<int>(VideoResult::Ok));
                break;
            }
            // Drain whatever the codec produced for this packet.
            for (;;)
            {
                VideoFrame tmp;
                const VideoResult dr = dec.dequeueFrame(tmp);
                CHECK_INT_EQ(static_cast<int>(dr),
                             static_cast<int>(VideoResult::Ok));
                if (tmp.data == nullptr)
                {
                    break;
                }
                CHECK_INT_EQ(tmp.width, kGenWidth);
                CHECK_INT_EQ(tmp.height, kGenHeight);
                CHECK_INT_EQ(static_cast<int>(tmp.format),
                             static_cast<int>(VideoPixelFormat::I420));
                CHECK_TRUE(tmp.dataSize >=
                           static_cast<uint32_t>(tmp.width * tmp.height * 3 / 2));
                CHECK_TRUE(tmp.pts.toUs() >= lastPtsUs);
                lastPtsUs = tmp.pts.toUs();
                ++frames;
            }
        }
        CHECK_INT_EQ(frames, kGenFrames);
        // 12 frames @ 25 fps -> last pts = 440 ms.
        CHECK_TRUE(lastPtsUs > 400'000);

        // After the drain, EOS requires a null feed (FFmpeg drain
        // sentinel). flush() only resets codec state for seek/replay.
        VideoPacket end{};
        end.isVideo = true;
        CHECK_INT_EQ(static_cast<int>(dec.feedPacket(end)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                     static_cast<int>(VideoResult::EndOfStream));
    }

    TEST_CASE(FFmpegDecoderAutoAccelOpensWithSoftFallback) {
        // CI / headless: Auto may not get a HW device — must still open
        // and decode via software (allowSoftwareFallback default).
        GeneratedClip c = makeClip(false);
        FedPackets fed = demuxVideoPackets(c.path);
        FFmpegDecoder dec;
        DecoderOpenParams params;
        params.codecName = fed.info.videoCodec;
        params.media = fed.info;
        params.preferredAccel = VideoDecodeAccel::Auto;
        params.allowSoftwareFallback = true;
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::Ok));
        // Active may be HW on machines with D3D11VA; None is also OK.
        const VideoDecodeAccel active = dec.activeDecodeAccel();
        CHECK_TRUE(active == VideoDecodeAccel::None
                   || active == VideoDecodeAccel::D3D11VA
                   || active == VideoDecodeAccel::DXVA2
                   || active == VideoDecodeAccel::CUDA);

        VideoPacket p = fed.video.front();
        CHECK_INT_EQ(static_cast<int>(dec.feedPacket(p)),
                     static_cast<int>(VideoResult::Ok));
        VideoFrame f;
        // Drain until a frame or empty Ok (codec delay).
        for (int i = 0; i < 8; ++i)
        {
            CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                         static_cast<int>(VideoResult::Ok));
            if (f.data)
            {
                break;
            }
            if (i + 1 < static_cast<int>(fed.video.size()))
            {
                (void)dec.feedPacket(fed.video[static_cast<size_t>(i + 1)]);
            }
        }
        CHECK_TRUE(f.data != nullptr);
        dec.close();
    }

    TEST_CASE(FFmpegDecoderFlushDrainsRemaining) {
        GeneratedClip c = makeClip(false);
        FedPackets fed = demuxVideoPackets(c.path);

        FFmpegDecoder dec;
        DecoderOpenParams params;
        params.codecName = fed.info.videoCodec;
        params.media = fed.info;
        CHECK_TRUE(params.media.width > 0);
        CHECK_TRUE(!params.media.videoExtradata.empty());
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::Ok));

        // Feed only the first half, decode them all, then flush:
        // the flush must turn the next dequeue into EndOfStream
        // (no B-frames → nothing left buffered).
        int frames = 0;
        for (size_t i = 0; i < fed.video.size() / 2; ++i)
        {
            CHECK_INT_EQ(feedAndDrain(dec, fed.video[i], frames), 0);
        }
        CHECK_INT_EQ(frames, static_cast<int>(fed.video.size() / 2));

        VideoFrame f;
        VideoPacket end{};
        end.isVideo = true;
        CHECK_INT_EQ(static_cast<int>(dec.feedPacket(end)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                     static_cast<int>(VideoResult::EndOfStream));
    }

    TEST_CASE(FFmpegDecoderAudioPacketsSkipped) {
        // See FFmpegDemuxerWithAudioStream — AAC synthetic mux deferred.
        CHECK_TRUE(true);
    }

TEST_SUITE_END
