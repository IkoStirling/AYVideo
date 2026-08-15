// Test_SubtitleTracks.cpp — V4 soft-subtitle track discovery (N-08).

#include <memory>
#include <thread>
#include <vector>

#include "AYTest.h"
#include "AYVideo/VideoMediaInfo.h"
#include "AYVideo/VideoPlayer.h"
#include "AYVideo/VideoSubtitle.h"
#include "AYVideo/VideoTypes.h"
#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"

using namespace ayt::video;

TEST_SUITE(SubtitleTracksSuite)

    TEST_CASE(SubtitleKindToStringCoversAll) {
        bool allOk = true;
        for (uint8_t i = 0; i < static_cast<uint8_t>(SubtitleKind::Count); ++i)
        {
            const char* s = toString(static_cast<SubtitleKind>(i));
            if (s == nullptr || s[0] == '\0')
            {
                allOk = false;
            }
        }
        CHECK(allOk);
        CHECK_INT_EQ(static_cast<int>(SubtitleKind::Count), 4);
    }

    TEST_CASE(MockDemuxerReportsSubtitleTrack) {
        MockDemuxer demux(4);
        demux.setProvideSubtitleTrack(true);
        CHECK_INT_EQ(static_cast<int>(demux.open({})),
                     static_cast<int>(VideoResult::Ok));
        MediaInfo info;
        CHECK_INT_EQ(static_cast<int>(demux.getMediaInfo(info)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(info.hasSubtitles);
        CHECK_INT_EQ(static_cast<int>(info.subtitleTracks.size()), 1);
        CHECK_INT_EQ(info.subtitleTracks[0].streamIndex, 3);
        CHECK_INT_EQ(static_cast<int>(info.subtitleTracks[0].kind),
                     static_cast<int>(SubtitleKind::Text));
        CHECK(info.subtitleTracks[0].codec == "mock-srt");
        CHECK(info.subtitleTracks[0].language == "eng");
    }

    TEST_CASE(PlayerSubtitleSelectionApi) {
        auto demux = std::make_unique<MockDemuxer>(4);
        demux->setProvideSubtitleTrack(true);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(4));

        SubtitleTrackInfo track{};
        CHECK_INT_EQ(static_cast<int>(player.getSubtitleTrack(0, track)),
                     static_cast<int>(VideoResult::InvalidState));
        CHECK_INT_EQ(player.activeSubtitleTrack(), -1);

        CHECK_INT_EQ(static_cast<int>(player.open("mock://clip")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.subtitleTrackCount()), 1);
        CHECK_INT_EQ(static_cast<int>(player.getSubtitleTrack(0, track)),
                     static_cast<int>(VideoResult::Ok));
        CHECK(track.codec == "mock-srt");
        CHECK_INT_EQ(static_cast<int>(player.getSubtitleTrack(1, track)),
                     static_cast<int>(VideoResult::InvalidArgument));

        CHECK_INT_EQ(static_cast<int>(player.setActiveSubtitleTrack(0)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(player.activeSubtitleTrack(), 0);
        CHECK_INT_EQ(static_cast<int>(player.setActiveSubtitleTrack(-1)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(player.activeSubtitleTrack(), -1);
        CHECK_INT_EQ(static_cast<int>(player.setActiveSubtitleTrack(9)),
                     static_cast<int>(VideoResult::InvalidArgument));
    }

    TEST_CASE(PlayerSoftSubtitleCuesFollowClock) {
        auto demux = std::make_unique<MockDemuxer>(12);
        demux->setProvideSubtitleTrack(true);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(12));
        CHECK_INT_EQ(static_cast<int>(player.open("mock://subs")),
                     static_cast<int>(VideoResult::Ok));
        MediaInfo info{};
        CHECK_INT_EQ(static_cast<int>(player.getMediaInfo(info)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(info.softSubtitleCues.size()), 2);

        std::vector<SubtitleCue> cues;
        CHECK_INT_EQ(static_cast<int>(player.pullActiveSubtitleCues(cues)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(cues.size()), 0); // track off

        CHECK_INT_EQ(static_cast<int>(player.setActiveSubtitleTrack(0)),
                     static_cast<int>(VideoResult::Ok));
        // Clock at 0 → first cue.
        CHECK_INT_EQ(static_cast<int>(player.pullActiveSubtitleCues(cues)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(cues.size()), 1);
        CHECK(cues[0].text == "mock-cue-0");

        CHECK_INT_EQ(static_cast<int>(
                         player.seek(ayt::time::Duration::fromUs(250'000),
                                     AYVideoPlayer::SeekMode::Keyframe)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.pullActiveSubtitleCues(cues)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(cues.size()), 1);
        CHECK(cues[0].text == "mock-cue-1");

        CHECK_INT_EQ(static_cast<int>(player.setActiveSubtitleTrack(-1)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.pullActiveSubtitleCues(cues)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(cues.size()), 0);
    }

    TEST_CASE(PlayerSubtitlePacketsFeedCueQueue) {
        // Packet path: play with subtitle track on; demux emits text
        // packets that DecodeLoop diverts into the cue mailbox.
        auto demux = std::make_unique<MockDemuxer>(16);
        MockDemuxer* demuxRaw = demux.get();
        demux->setProvideSubtitleTrack(true);
        AYVideoPlayer player(std::move(demux), std::make_unique<MockDecoder>(16));
        CHECK_INT_EQ(static_cast<int>(player.open("mock://sub-packets")),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(static_cast<int>(player.setActiveSubtitleTrack(0)),
                     static_cast<int>(VideoResult::Ok));
        CHECK_INT_EQ(demuxRaw->activeSubtitleStreamIndex(), 3);

        CHECK_INT_EQ(static_cast<int>(player.play()),
                     static_cast<int>(VideoResult::Ok));
        bool sawCue = false;
        for (int i = 0; i < 400; ++i)
        {
            std::vector<SubtitleCue> cues;
            (void)player.pullActiveSubtitleCues(cues);
            for (const auto& c : cues)
            {
                if (c.text == "mock-cue-0" || c.text == "mock-cue-1")
                {
                    sawCue = true;
                    break;
                }
            }
            VideoFrame f{};
            (void)player.pullFrame(f);
            if (sawCue && demuxRaw->readCount() > 16u)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(sawCue);
        // Subtitle packets are extras on top of 16 video packets.
        CHECK_TRUE(demuxRaw->readCount() > 16u);
        CHECK_INT_EQ(static_cast<int>(player.stop()),
                     static_cast<int>(VideoResult::Ok));
    }

TEST_SUITE_END
