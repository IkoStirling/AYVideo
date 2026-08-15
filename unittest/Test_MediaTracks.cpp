// Test_MediaTracks.cpp — V4 N-10 A/V multi-track discovery + selection.

#include <memory>

#include "AYTest.h"
#include "AYVideo/VideoMediaInfo.h"
#include "AYVideo/VideoPlayer.h"
#include "AYVideo/VideoTrack.h"
#include "AYVideo/VideoTypes.h"
#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"

using namespace ayt::video;

TEST_SUITE(MediaTracksSuite)

    TEST_CASE(MockDemuxerReportsDualAudioTracks) {
        MockDemuxer demux(4);
        demux.setProvideMultiAudio(true);
        CHECK_INT_EQ(static_cast<int>(demux.open({})),
                     static_cast<int>(VideoResult::Ok));
        MediaInfo info;
        CHECK_INT_EQ(static_cast<int>(demux.getMediaInfo(info)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(info.hasAudio);
        CHECK_INT_EQ(static_cast<int>(info.audioTracks.size()), 2);
        CHECK_INT_EQ(static_cast<int>(info.videoTracks.size()), 1);
        CHECK(info.audioTracks[0].language == "eng");
        CHECK(info.audioTracks[1].language == "jpn");
        CHECK_INT_EQ(info.audioTracks[0].streamIndex, 1);
        CHECK_INT_EQ(info.audioTracks[1].streamIndex, 2);
    }

    TEST_CASE(PlayerAudioTrackSelectionBounds) {
        auto demux = std::make_unique<MockDemuxer>(4);
        demux->setProvideMultiAudio(true);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(4));
        CHECK_INT_EQ(static_cast<int>(player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.audioTrackCount()), 2);
        CHECK_INT_EQ(static_cast<int>(player.videoTrackCount()), 1);
        CHECK_INT_EQ(player.activeAudioTrack(), 0);
        CHECK_INT_EQ(player.activeVideoTrack(), 0);

        AudioTrackInfo at{};
        CHECK_INT_EQ(static_cast<int>(player.getAudioTrack(1, at)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(at.language == "jpn");
        CHECK_INT_EQ(static_cast<int>(player.getAudioTrack(9, at)),
                     static_cast<int>(VideoResult::InvalidArgument));
        CHECK_INT_EQ(static_cast<int>(player.setActiveAudioTrack(1)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(player.activeAudioTrack(), 1);
        CHECK_INT_EQ(static_cast<int>(player.setActiveAudioTrack(-1)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(player.activeAudioTrack(), -1);
        CHECK_INT_EQ(static_cast<int>(player.setActiveVideoTrack(-1)),
                     static_cast<int>(VideoResult::InvalidArgument));
    }

    TEST_CASE(PlayerApplyActiveAudioOnSeek) {
        auto demux = std::make_unique<MockDemuxer>(8);
        auto* demuxRaw = demux.get();
        demux->setProvideMultiAudio(true);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(8));
        CHECK_INT_EQ(static_cast<int>(player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.setActiveAudioTrack(1)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(demuxRaw->activeAudioStreamIndex(), 2); // jpn stream
        CHECK_INT_EQ(static_cast<int>(player.pause()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.setActiveAudioTrack(0)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(
                         player.seek(ayt::time::Duration::fromUs(0))),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(demuxRaw->activeAudioStreamIndex(), 1); // eng stream
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

TEST_SUITE_END
