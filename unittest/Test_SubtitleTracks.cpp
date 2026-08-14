// Test_SubtitleTracks.cpp — V4 soft-subtitle track discovery (N-08).

#include <memory>

#include "AYTest.h"
#include "AYVideoMediaInfo.h"
#include "AYVideoPlayer.h"
#include "AYVideoSubtitle.h"
#include "AYVideoTypes.h"
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

TEST_SUITE_END
