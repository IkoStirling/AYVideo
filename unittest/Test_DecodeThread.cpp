// Test_DecodeThread.cpp — V1.
//
// §16.3 / §14 stress: 400 decoded frames × 3 rounds, bit-identical
// across rounds ("逐位一致", AYAnimation P4 stress precedent). A 400
// frame @ 25 fps (16 s) synthetic clip exercises the full pipeline:
// decode thread + SPSC backpressure + clock-gated presentation.
//
// Determinism: the mpeg4 encoder and the decoder are both deterministic
// (Q7a), so frame N's pixel bytes must be identical in every round.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AYTest.h"
#include "AYVideo/VideoPlayer.h"
#include "AYVideo/VideoTypes.h"
#include "FFmpegTestMedia.h"
#include "../backend/FFmpegDecoder.h"
#include "../backend/FFmpegDemuxer.h"

#include <AYTime/TimePoint.h>

using namespace ayt::video;
using namespace ayt::testmedia;

namespace
{

constexpr int32_t kStressFrames = 400;

struct FakeNow
{
    static std::int64_t us;

    static ayt::time::TimePoint tick() noexcept
    {
        return ayt::time::TimePoint::fromUnixUs(us);
    }
};

std::int64_t FakeNow::us = 0;

VideoResult pullWithRetry(AYVideoPlayer& p, VideoFrame& out,
                          int maxTries = 5000)
{
    for (int i = 0; i < maxTries; ++i)
    {
        const VideoResult r = p.pullFrame(out);
        if (r != VideoResult::Ok || out.data != nullptr)
        {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return VideoResult::Ok;
}

// One full playback round: pull kStressFrames frames in pts order,
// copying each frame's bytes. Returns false (with the failure on
// stderr) if anything misbehaves.
bool playRound(const std::string& path,
               std::vector<std::vector<uint8_t>>& framesOut)
{
    FakeNow::us = 0;
    AYVideoPlayer player(std::make_unique<FFmpegDemuxer>(),
                         std::make_unique<FFmpegDecoder>(),
                         &FakeNow::tick);
    if (player.open(path) != VideoResult::Ok)
    {
        fprintf(stderr, "[stress] open failed\n");
        return false;
    }
    if (player.play() != VideoResult::Ok)
    {
        fprintf(stderr, "[stress] play failed\n");
        return false;
    }

    framesOut.clear();
    framesOut.reserve(kStressFrames);
    std::int64_t lastPtsUs = -1;
    for (int i = 0; i < kStressFrames; ++i)
    {
        FakeNow::us += 40'000; // 25 fps
        VideoFrame f;
        const VideoResult r = pullWithRetry(player, f);
        if (r != VideoResult::Ok || f.data == nullptr)
        {
            fprintf(stderr, "[stress] frame %d missing (r=%d)\n", i,
                    static_cast<int>(r));
            player.stop();
            return false;
        }
        if (f.pts.toUs() < lastPtsUs)
        {
            fprintf(stderr, "[stress] pts regression at %d\n", i);
            return false;
        }
        lastPtsUs = f.pts.toUs();
        framesOut.emplace_back(f.data, f.data + f.dataSize);
    }

    // EOS must follow.
    FakeNow::us += 40'000;
    VideoFrame f;
    const VideoResult r = pullWithRetry(player, f);
    if (r != VideoResult::EndOfStream)
    {
        fprintf(stderr, "[stress] no EOS (r=%d)\n", static_cast<int>(r));
        return false;
    }
    if (player.state() != PlayerState::Ready)
    {
        fprintf(stderr, "[stress] state %d after EOS\n",
                static_cast<int>(player.state()));
        player.stop();
        return false;
    }
    player.stop();
    return true;
}

} // namespace

TEST_SUITE(DecodeThreadSuite)

    TEST_CASE(DecodeStress400FramesThreeRoundsBitIdentical) {
        // Generate the 400-frame clip once (deterministic encoder).
        std::string err;
        const std::string path = tempClipPath("stress400");
        if (!generateClip(path, false, kStressFrames, kGenWidth, kGenHeight,
                          kGenFps, err))
        {
            fprintf(stderr, "[stress] generation failed: %s\n", err.c_str());
            CHECK_TRUE(false);
            return;
        }

        std::vector<std::vector<uint8_t>> round1;
        CHECK_TRUE(playRound(path, round1));
        CHECK_INT_EQ(static_cast<int>(round1.size()), kStressFrames);
        CHECK_TRUE(round1.front().size() >=
                   static_cast<size_t>(kGenWidth * kGenHeight * 3 / 2));

        // Rounds 2 and 3 must be bit-identical to round 1 (frame by
        // frame — the same decode timeline every time).
        for (int round = 2; round <= 3; ++round)
        {
            std::vector<std::vector<uint8_t>> next;
            CHECK_TRUE(playRound(path, next));
            CHECK_INT_EQ(static_cast<int>(next.size()), kStressFrames);
            bool identical = true;
            for (int i = 0; i < kStressFrames; ++i)
            {
                if (next[i] != round1[i])
                {
                    fprintf(stderr,
                            "[stress] round %d frame %d differs (%zu vs %zu)\n",
                            round, i, next[i].size(), round1[i].size());
                    identical = false;
                    break;
                }
            }
            CHECK_TRUE(identical);
        }
    }

TEST_SUITE_END
