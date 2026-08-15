// Test_MockBackends.cpp — V0.5 stub.
//
// Asserts the Mock backend contract (design.md §17): scripted packet /
// frame sequences, deterministic payloads, EndOfStream at the end of
// the script, interaction counters for player↔backend contract tests.

#include "AYTest.h"
#include "AYVideoTypes.h"
#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"

using namespace ayt::video;

TEST_SUITE(MockBackendsSuite)

    TEST_CASE(MockDemuxerReplaysPacketSequence) {
        MockDemuxer d(3);
        DemuxerOpenParams params;
        params.path = "mock://seq";
        CHECK_INT_EQ(static_cast<int>(d.open(params)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(d.openCount()), 1u);

        MediaInfo info;
        CHECK_INT_EQ(static_cast<int>(d.getMediaInfo(info)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(info.hasVideo);
        CHECK_INT_EQ(info.width, 320);
        CHECK_INT_EQ(info.height, 240);
        CHECK_FLOAT_EQ(static_cast<float>(info.frameRate), 25.0f, 1e-9f);

        VideoPacket p;
        for (int i = 0; i < 3; ++i) {
            CHECK_INT_EQ(static_cast<int>(d.readNextPacket(p)),
                         static_cast<int>(VideoResult::Ok));
            CHECK_NOT_NULL(p.data);
            CHECK_TRUE(p.isVideo);
            // Deterministic payload: first byte = packet index.
            CHECK_INT_EQ(static_cast<int>(p.data[0]), i);
            CHECK_INT_EQ(static_cast<int>(p.pts.toUs()), i * 40'000);
        }
        CHECK_INT_EQ(static_cast<int>(d.readNextPacket(p)),
                     static_cast<int>(VideoResult::EndOfStream));
        CHECK_INT_EQ(static_cast<int>(d.readCount()), 3u);
    }

    TEST_CASE(MockDemuxerSeekRestartsSequence) {
        MockDemuxer d(5);
        DemuxerOpenParams params;
        CHECK_INT_EQ(static_cast<int>(d.open(params)),
                     static_cast<int>(VideoResult::Ok));

        VideoPacket p;
        for (int i = 0; i < 3; ++i) {
            CHECK_INT_EQ(static_cast<int>(d.readNextPacket(p)),
                         static_cast<int>(VideoResult::Ok));
        }
        CHECK_INT_EQ(static_cast<int>(d.seek(ayt::time::Duration::fromMs(0))),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(d.seekCount()), 1u);
        // After seek(0) the sequence restarts from the beginning.
        CHECK_INT_EQ(static_cast<int>(d.readNextPacket(p)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(p.data[0]), 0);
    }

    TEST_CASE(MockDemuxerSeekJumpsToTargetPts) {
        MockDemuxer d(10);
        DemuxerOpenParams params;
        CHECK_INT_EQ(static_cast<int>(d.open(params)),
                     static_cast<int>(VideoResult::Ok));
        // 200 ms @ 25 fps → packet index 5.
        CHECK_INT_EQ(static_cast<int>(
                         d.seek(ayt::time::Duration::fromUs(200'000))),
                     static_cast<int>(VideoResult::Ok));
        VideoPacket p;
        CHECK_INT_EQ(static_cast<int>(d.readNextPacket(p)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(p.data[0]), 5);
        CHECK_INT_EQ(static_cast<int>(p.pts.toUs()), 200'000);
    }

    TEST_CASE(MockDecoderReplaysFrameSequence) {
        MockDecoder dec(2);
        DecoderOpenParams params;
        params.codecName = "mock-h264";
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::Ok));

        // No packets fed yet -> "no frame ready yet" contract state.
        VideoFrame f;
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_NULL(f.data);

        VideoPacket p;
        CHECK_INT_EQ(static_cast<int>(dec.feedPacket(p)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(dec.feedCount()), 1u);

        for (int i = 0; i < 2; ++i) {
            CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                         static_cast<int>(VideoResult::Ok));
            CHECK_NOT_NULL(f.data);
            CHECK_INT_EQ(f.width, 320);
            CHECK_INT_EQ(f.height, 240);
            CHECK_INT_EQ(static_cast<int>(f.format),
                         static_cast<int>(VideoPixelFormat::RGBA8));
            CHECK_INT_EQ(static_cast<int>(f.pts.toUs()), i * 40'000);
            CHECK_INT_EQ(f.dataSize, 320u * 240u * 4u);
        }

        // Script exhausted: Ok (not EOS) until flush().
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_NULL(f.data);
        CHECK_INT_EQ(static_cast<int>(dec.flush()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(dec.flushCount()), 1u);
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(f)),
                     static_cast<int>(VideoResult::EndOfStream));
    }

    TEST_CASE(MockBackendCloseMarksClosed) {
        MockDemuxer d(1);
        DemuxerOpenParams params;
        CHECK_INT_EQ(static_cast<int>(d.open(params)),
                     static_cast<int>(VideoResult::Ok));
        d.close();
        CHECK_TRUE(d.wasClosed());
        CHECK_FALSE(d.isOpen());

        MockDecoder dec(1);
        DecoderOpenParams decParams;
        CHECK_INT_EQ(static_cast<int>(dec.open(decParams)),
                     static_cast<int>(VideoResult::Ok));
        dec.close();
        CHECK_TRUE(dec.wasClosed());
        CHECK_FALSE(dec.isOpen());
    }

TEST_SUITE_END
