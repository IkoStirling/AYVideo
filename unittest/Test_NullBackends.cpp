// Test_NullBackends.cpp — V0.5 stub.
//
// Asserts the Null backend contract (design.md §17): silent no-op
// semantics with coherent open/close/isOpen state; readNextPacket
// reports EndOfStream immediately; dequeueFrame reports the
// "no frame ready yet" contract state; flush gates EndOfStream.

#include "AYTest.h"
#include "AYVideoTypes.h"
#include "../backend/NullDecoder.h"
#include "../backend/NullDemuxer.h"

using namespace ayt::video;

TEST_SUITE(NullBackendsSuite)

    TEST_CASE(NullDemuxerOpenCloseContract) {
        NullDemuxer d;
        CHECK_FALSE(d.isOpen());
        // Operations before open must fail cleanly.
        MediaInfo info;
        VideoPacket packet;
        CHECK_INT_EQ(static_cast<int>(d.getMediaInfo(info)),
                     static_cast<int>(VideoResult::NotInitialized));
        CHECK_INT_EQ(static_cast<int>(d.readNextPacket(packet)),
                     static_cast<int>(VideoResult::NotInitialized));
        CHECK_INT_EQ(static_cast<int>(d.seek(ayt::time::Duration::fromMs(500))),
                     static_cast<int>(VideoResult::NotInitialized));

        DemuxerOpenParams params;
        params.path = "null://";
        CHECK_INT_EQ(static_cast<int>(d.open(params)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(d.isOpen());

        // After open: metadata is a zeroed snapshot; no packets.
        CHECK_INT_EQ(static_cast<int>(d.getMediaInfo(info)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_FALSE(info.hasVideo);
        CHECK_INT_EQ(static_cast<int>(d.readNextPacket(packet)),
                     static_cast<int>(VideoResult::EndOfStream));
        CHECK_INT_EQ(static_cast<int>(d.seek(ayt::time::Duration::fromMs(500))),
                     static_cast<int>(VideoResult::Ok));

        d.close();
        CHECK_FALSE(d.isOpen());
    }

    TEST_CASE(NullDecoderNoFrameReadyContract) {
        NullDecoder dec;
        CHECK_FALSE(dec.isOpen());
        VideoFrame frame;
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(frame)),
                     static_cast<int>(VideoResult::NotInitialized));

        DecoderOpenParams params;
        CHECK_INT_EQ(static_cast<int>(dec.open(params)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_TRUE(dec.isOpen());

        // No frames ever: "no frame ready yet" (Ok + null frame), not EOS.
        VideoPacket packet;
        CHECK_INT_EQ(static_cast<int>(dec.feedPacket(packet)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(frame)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_NULL(frame.data);

        // flush() gates EndOfStream (design.md §8.3).
        CHECK_INT_EQ(static_cast<int>(dec.flush()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(dec.dequeueFrame(frame)),
                     static_cast<int>(VideoResult::EndOfStream));

        dec.close();
        CHECK_FALSE(dec.isOpen());
    }

TEST_SUITE_END
