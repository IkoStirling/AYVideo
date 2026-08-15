#include "DecodeLoop.h"

#include "AudioQueue.h"
#include "FrameQueue.h"
#include "SeekLog.h"
#include "SubtitleCueQueue.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace ayt::video
{
namespace
{

VideoFrame reseatFrame(VideoFrame frame, std::vector<uint8_t>& pixels)
{
    if (!pixels.empty())
    {
        frame.data = pixels.data();
        frame.dataSize = static_cast<uint32_t>(pixels.size());
    }
    return frame;
}

// Best-effort text extract for soft subtitle packets (Mock UTF-8 or
// mov_text length-prefixed). ASS/bitmap left empty.
std::string subtitleTextFromPacket(const VideoPacket& pkt)
{
    if (!pkt.data || pkt.size == 0)
    {
        return {};
    }
    size_t off = 0;
    if (pkt.size >= 3 && pkt.data[0] == 0xEF && pkt.data[1] == 0xBB
        && pkt.data[2] == 0xBF)
    {
        off = 3;
    }
    // mov_text: big-endian u16 length + UTF-8 payload.
    if (pkt.size >= off + 2)
    {
        const uint16_t len = static_cast<uint16_t>(
            (static_cast<uint16_t>(pkt.data[off]) << 8)
            | static_cast<uint16_t>(pkt.data[off + 1]));
        if (len > 0 && static_cast<size_t>(off + 2 + len) <= pkt.size)
        {
            return std::string(
                reinterpret_cast<const char*>(pkt.data + off + 2), len);
        }
    }
    return std::string(reinterpret_cast<const char*>(pkt.data + off),
                       pkt.size - off);
}

void ingestSubtitlePacket(SubtitleCueQueue* queue, const VideoPacket& pkt)
{
    if (!queue)
    {
        return;
    }
    std::string text = subtitleTextFromPacket(pkt);
    // Trim trailing NULs from some containers.
    while (!text.empty() && text.back() == '\0')
    {
        text.pop_back();
    }
    if (text.empty())
    {
        return;
    }
    SubtitleCue cue{};
    cue.start = pkt.pts;
    if (pkt.duration.toUs() > 0)
    {
        cue.end = ayt::time::Duration::fromUs(pkt.pts.toUs()
                                              + pkt.duration.toUs());
    }
    else
    {
        cue.end = ayt::time::Duration::fromUs(pkt.pts.toUs() + 200'000);
    }
    cue.text = std::move(text);
    queue->push(std::move(cue));
}

} // namespace

DecodeLoop::DecodeLoop(IAYVideoDemuxer& demuxer, IAYVideoDecoder& decoder,
                       FrameQueue& videoQueue, AudioQueue* audioQueue,
                       DecodeLoopOptions options, SubtitleCueQueue* subtitleCues)
    : _demuxer(demuxer)
    , _decoder(decoder)
    , _videoQueue(videoQueue)
    , _audioQueue(audioQueue)
    , _subtitleCues(subtitleCues)
    , _options(options)
{
}

DecodeLoop::~DecodeLoop()
{
    requestStop();
    join();
}

void DecodeLoop::restoreOverflowPolicy() noexcept
{
    if (_overflowPolicySaved)
    {
        _videoQueue.setOverflowPolicy(
            static_cast<FrameQueueOverflowPolicy>(_savedOverflowPolicy));
        _overflowPolicySaved = false;
    }
}

void DecodeLoop::armScrubPreview(int64_t targetUs) noexcept
{
    _scrubActive.store(true, std::memory_order_release);
    _scrubCeilingUs.store(targetUs, std::memory_order_release);
    // Scrub walk ignores Accurate dropBelow; clear only when entering scrub.
    _dropBelowPtsUs.store(0, std::memory_order_release);
    _dropBelowArmedAt = {};
    if (!_overflowPolicySaved)
    {
        _savedOverflowPolicy =
            static_cast<uint8_t>(_videoQueue.overflowPolicy());
        _overflowPolicySaved = true;
    }
    _videoQueue.setOverflowPolicy(FrameQueueOverflowPolicy::DropOldest);
}

void DecodeLoop::armCatchUpOrPlaying(InLoopSeekMode mode,
                                     int64_t targetUs) noexcept
{
    _scrubActive.store(false, std::memory_order_release);
    _scrubPausedAtCeiling.store(false, std::memory_order_release);
    restoreOverflowPolicy();

    if (mode == InLoopSeekMode::Keyframe)
    {
        _dropBelowPtsUs.store(0, std::memory_order_release);
        _dropBelowArmedAt = {};
        return;
    }

    // Accurate (or Scrub converted to catch-up): always arm this seek's
    // target. Do not keep a stale prior-seek floor (e.g. 200ms then 80ms).
    _dropBelowPtsUs.store(targetUs > 0 ? targetUs : 0,
                          std::memory_order_release);
    if (targetUs > 0)
    {
        _dropBelowArmedAt = std::chrono::steady_clock::now();
    }
    else
    {
        _dropBelowArmedAt = {};
    }
}

void DecodeLoop::start()
{
    _cancel.store(false);
    _finished.store(false);
    _endedClean.store(false);
    _failure.store(VideoResult::Ok);
    _skippedErrors.store(0);
    _reconnectAttempts.store(0);
    _seekSerial.store(0);
    _seekAppliedSerial.store(0);
    _seekResult.store(VideoResult::Ok);
    _dropBelowPtsUs.store(0);
    _postSeekMinPtsUs.store(0);
    _intent.store(static_cast<uint8_t>(PlaybackIntent::Playing),
                  std::memory_order_relaxed);
    _scrubActive.store(false);
    _scrubPausedAtCeiling.store(false);
    _scrubCeilingUs.store(0);
    _scrubLandPtsUs.store(0);
    _scrubLastOutPtsUs.store(0);
    _pendingOverCeiling.reset();
    _overflowPolicySaved = false;
    // Previous loop's requestStop() left demuxer abort latched — clear
    // so av_read_frame is not permanently short-circuited.
    _demuxer.clearAbort();
    _thread = std::thread(&DecodeLoop::run, this);
}

void DecodeLoop::requestStop() noexcept
{
    _cancel.store(true);
    _demuxer.requestAbort();
}

void DecodeLoop::join() noexcept
{
    if (_thread.joinable())
    {
        _thread.join();
    }
}

uint64_t DecodeLoop::requestSeek(const ayt::time::Duration& target,
                                 InLoopSeekMode mode, int32_t videoStreamIndex,
                                 int32_t audioStreamIndex) noexcept
{
    _seekTargetUs.store(target.toUs(), std::memory_order_relaxed);
    _seekMode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
    _seekVideoStream.store(videoStreamIndex, std::memory_order_relaxed);
    _seekAudioStream.store(audioStreamIndex, std::memory_order_relaxed);
    if (mode == InLoopSeekMode::Scrub)
    {
        // Do not clobber CatchUpToFloor if play() already committed.
        if (intent() != PlaybackIntent::CatchUpToFloor)
        {
            _intent.store(static_cast<uint8_t>(PlaybackIntent::ScrubPreview),
                          std::memory_order_release);
        }
        _scrubCeilingUs.store(target.toUs(), std::memory_order_relaxed);
        _scrubPausedAtCeiling.store(false, std::memory_order_relaxed);
    }
    else if (mode == InLoopSeekMode::Accurate)
    {
        // Accurate seek is a catch-up walk unless already Playing steady.
        if (intent() != PlaybackIntent::Playing)
        {
            _intent.store(static_cast<uint8_t>(PlaybackIntent::CatchUpToFloor),
                          std::memory_order_release);
        }
    }
    const uint64_t serial =
        _seekSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    _demuxer.requestAbort();
    return serial;
}

bool DecodeLoop::updateScrubTarget(const ayt::time::Duration& target) noexcept
{
    if (intent() != PlaybackIntent::ScrubPreview
        || !_scrubActive.load(std::memory_order_acquire))
    {
        return false;
    }
    const std::int64_t targetUs = target.toUs();
    const std::int64_t land =
        _scrubLandPtsUs.load(std::memory_order_acquire);
    const std::int64_t last =
        _scrubLastOutPtsUs.load(std::memory_order_acquire);
    const std::int64_t curTarget =
        _seekTargetUs.load(std::memory_order_relaxed);
    // ENDTRACK often re-seeks the same thumb — treat as ceiling bump, not
    // a second demux (that raced play.catchUp and left stale high-pts frames).
    if (curTarget > 0 && targetUs >= curTarget - 40'000
        && targetUs <= curTarget + 40'000)
    {
        _scrubCeilingUs.store(targetUs, std::memory_order_release);
        _seekTargetUs.store(targetUs, std::memory_order_relaxed);
        _scrubPausedAtCeiling.store(false, std::memory_order_release);
        return true;
    }
    // Still before the session's keyframe land — need a real re-seek.
    if (land > 0 && targetUs + 40'000 < land)
    {
        return false;
    }
    // Small backward twitch near lastOut: keep ceiling at lastOut (extend)
    // instead of a full KF re-seek that races ENDTRACK+play.
    constexpr std::int64_t kSmallBackUs = 500'000; // 0.5 s
    if (last > 0 && targetUs + 120'000 < last)
    {
        if (last - targetUs <= kSmallBackUs)
        {
            _scrubCeilingUs.store(last, std::memory_order_release);
            _seekTargetUs.store(last, std::memory_order_relaxed);
            _scrubPausedAtCeiling.store(false, std::memory_order_release);
            return true;
        }
        return false;
    }
    // Far ahead of decoded output: walking every frame feels like a
    // multi-second buffer stall. Force a demux KF near the new thumb.
    constexpr std::int64_t kMaxForwardWalkUs = 2'500'000; // 2.5 s
    if (last > 0 && targetUs > last + kMaxForwardWalkUs)
    {
        return false;
    }
    if (last <= 0 && land > 0 && targetUs > land + kMaxForwardWalkUs)
    {
        return false;
    }
    _scrubCeilingUs.store(targetUs, std::memory_order_release);
    _seekTargetUs.store(targetUs, std::memory_order_relaxed);
    _scrubPausedAtCeiling.store(false, std::memory_order_release);
    return true;
}

void DecodeLoop::endScrubMode() noexcept
{
    // Player-safe: clear ceiling pause so decode wakes. Pending over-
    // ceiling flush is decode-thread only (SPSC producer).
    _scrubActive.store(false, std::memory_order_release);
    _scrubPausedAtCeiling.store(false, std::memory_order_release);
    restoreOverflowPolicy();
}

bool DecodeLoop::waitSeekApplied(uint64_t serial,
                                 uint32_t timeoutMs) const noexcept
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (_seekAppliedSerial.load(std::memory_order_acquire) < serial)
    {
        if (_cancel.load(std::memory_order_relaxed)
            || _finished.load(std::memory_order_relaxed))
        {
            return _seekAppliedSerial.load(std::memory_order_acquire) >= serial;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool DecodeLoop::applyPendingSeek() noexcept
{
    uint64_t serial = _seekSerial.load(std::memory_order_acquire);
    if (serial == _seekAppliedSerial.load(std::memory_order_acquire))
    {
        return false;
    }

    // Coalesce: keep re-seeking until the latest serial is applied.
    for (;;)
    {
        const auto t0 = std::chrono::steady_clock::now();
        serial = _seekSerial.load(std::memory_order_acquire);
        const int64_t targetUs =
            _seekTargetUs.load(std::memory_order_relaxed);
        auto mode = static_cast<InLoopSeekMode>(
            _seekMode.load(std::memory_order_relaxed));
        const int32_t vStream =
            _seekVideoStream.load(std::memory_order_relaxed);
        const int32_t aStream =
            _seekAudioStream.load(std::memory_order_relaxed);

        if (seeklog::enabled())
        {
            const char* modeName = "Accurate";
            if (mode == InLoopSeekMode::Keyframe)
            {
                modeName = "Keyframe";
            }
            else if (mode == InLoopSeekMode::Scrub)
            {
                modeName = "Scrub";
            }
            std::fprintf(stderr,
                         "[AYVideo][seek] decode.apply serial=%llu target=%.3fs "
                         "mode=%s intent=%u\n",
                         static_cast<unsigned long long>(serial),
                         targetUs / 1e6, modeName,
                         static_cast<unsigned>(_intent.load(
                             std::memory_order_relaxed)));
            std::fflush(stderr);
        }

        _demuxer.clearAbort();
        const auto tTracks = std::chrono::steady_clock::now();
        // Skip invalid indices (video required). A failed call must not
        // poison `_applied*` or the next seek will keep retrying -1 and
        // surface InvalidArgument mid-scrub.
        if (vStream >= 0
            && (vStream != _appliedVideoStream || aStream != _appliedAudioStream))
        {
            const VideoResult tr =
                _demuxer.setActiveStreamIndices(vStream, aStream);
            if (tr == VideoResult::Ok)
            {
                _appliedVideoStream = vStream;
                _appliedAudioStream = aStream;
                seeklog::stage("decode.setStreams", seeklog::msSince(tTracks));
            }
            else
            {
                std::fprintf(stderr,
                             "[AYVideo][seek] decode.setStreams failed: %s "
                             "v=%d a=%d\n",
                             toString(tr), vStream, aStream);
                std::fflush(stderr);
            }
        }

        const auto tSeek = std::chrono::steady_clock::now();
        // Always land on a keyframe. Accurate/Scrub then walk keyframe→
        // target; Keyframe presents that I-frame only.
        const VideoResult r =
            _demuxer.seek(ayt::time::Duration::fromUs(targetUs),
                          /*keyframeOnly=*/true);
        seeklog::stage("decode.demuxSeek", seeklog::msSince(tSeek));

        const auto tFlush = std::chrono::steady_clock::now();
        (void)_decoder.flush();
        seeklog::stage("decode.flush", seeklog::msSince(tFlush));

        _seekResult.store(r, std::memory_order_release);
        _seekAppliedSerial.store(serial, std::memory_order_release);
        _endedClean.store(false);
        _logNextVideoPts = true;
        _postSeekMinPtsUs.store(std::numeric_limits<std::int64_t>::max(),
                                std::memory_order_release);
        _scrubLandPtsUs.store(0, std::memory_order_relaxed);
        _scrubLastOutPtsUs.store(0, std::memory_order_relaxed);
        _scrubPausedAtCeiling.store(false, std::memory_order_relaxed);
        _pendingOverCeiling.reset();

        // Re-read intent AFTER demux/flush — play() may have committed
        // CatchUp while this Scrub seek was in flight.
        const PlaybackIntent intentNow = intent();
        if (mode == InLoopSeekMode::Scrub
            && intentNow == PlaybackIntent::ScrubPreview)
        {
            armScrubPreview(targetUs);
        }
        else if (mode == InLoopSeekMode::Scrub
                 && (intentNow == PlaybackIntent::CatchUpToFloor
                     || intentNow == PlaybackIntent::Playing))
        {
            // In-flight Scrub converted by play(): keep player-armed floor.
            const std::int64_t existing =
                _dropBelowPtsUs.load(std::memory_order_relaxed);
            armCatchUpOrPlaying(
                InLoopSeekMode::Accurate,
                (existing > 0) ? existing : targetUs);
        }
        else
        {
            // Accurate / Keyframe: always this seek's target (never a
            // stale prior-seek dropBelow).
            armCatchUpOrPlaying(mode, targetUs);
        }

        seeklog::stage("decode.apply.total", seeklog::msSince(t0));

        if (_seekSerial.load(std::memory_order_acquire) == serial)
        {
            return true;
        }
        if (_cancel.load(std::memory_order_relaxed))
        {
            return true;
        }
        // Newer seek posted while we were seeking — loop again.
        if (seeklog::enabled())
        {
            seeklog::event("decode.apply coalesced — redo");
        }
    }
}

void DecodeLoop::run() noexcept
{
    _running.store(true);

    VideoPacket pkt;
    VideoFrame frame;
    AudioPcmFrame audio;
    uint32_t reconnectUsed = 0;

    // Drain decode output into the queues until Ok+null / EOS / error /
    // cancel. Returns false when the outer loop must stop.
    auto pumpFrames = [&](bool& eosSeen) -> bool {
        for (;;)
        {
            if (_cancel.load())
            {
                return false;
            }
            if (_seekSerial.load(std::memory_order_relaxed)
                != _seekAppliedSerial.load(std::memory_order_relaxed))
            {
                // Abort drain so the outer loop can apply the seek.
                return true;
            }
            const VideoResult fr = _decoder.dequeueFrame(frame);
            if (fr == VideoResult::Ok && frame.data)
            {
                const std::int64_t ptsUs = frame.pts.toUs();
                const std::int64_t postMin =
                    _postSeekMinPtsUs.load(std::memory_order_relaxed);
                if (postMin != 0 && ptsUs < postMin)
                {
                    continue; // pre-seek / pre-land stale output
                }

                const PlaybackIntent intentNow = intent();
                const bool scrubPreview =
                    intentNow == PlaybackIntent::ScrubPreview
                    && _scrubActive.load(std::memory_order_relaxed);

                if (scrubPreview)
                {
                    _scrubLastOutPtsUs.store(ptsUs, std::memory_order_relaxed);
                    const std::int64_t ceil =
                        _scrubCeilingUs.load(std::memory_order_relaxed);
                    if (ptsUs > ceil)
                    {
                        // Stash one over-ceiling frame; do not destroy it.
                        PendingFrame pending;
                        pending.pixels.assign(frame.data,
                                              frame.data + frame.dataSize);
                        pending.frame = frame;
                        pending.frame.data = pending.pixels.data();
                        pending.seekSerial = _seekAppliedSerial.load(
                            std::memory_order_relaxed);
                        _pendingOverCeiling = std::move(pending);
                        _scrubPausedAtCeiling.store(true,
                                                    std::memory_order_release);
                        return true;
                    }
                    _scrubPausedAtCeiling.store(false,
                                                std::memory_order_relaxed);
                    _pendingOverCeiling.reset();
                    _videoQueue.push(
                        frame,
                        _seekAppliedSerial.load(std::memory_order_relaxed));
                    continue;
                }

                // CatchUpToFloor / Playing — dropBelow walk; never ceiling-pause.
                if (_scrubActive.load(std::memory_order_relaxed)
                    || _pendingOverCeiling)
                {
                    endScrubMode();
                    if (_pendingOverCeiling)
                    {
                        VideoFrame f = reseatFrame(
                            _pendingOverCeiling->frame,
                            _pendingOverCeiling->pixels);
                        _videoQueue.push(f, _pendingOverCeiling->seekSerial);
                        _pendingOverCeiling.reset();
                    }
                }

                const std::int64_t dropBelow =
                    _dropBelowPtsUs.load(std::memory_order_relaxed);
                if (dropBelow > 0 && ptsUs < dropBelow)
                {
                    continue; // still catching up to floor
                }
                if (dropBelow > 0)
                {
                    if (_dropBelowArmedAt.time_since_epoch().count() != 0)
                    {
                        seeklog::stage("decode.floor.ready",
                                       seeklog::msSince(_dropBelowArmedAt));
                        _dropBelowArmedAt = {};
                    }
                    _dropBelowPtsUs.store(0, std::memory_order_relaxed);
                    if (intentNow == PlaybackIntent::CatchUpToFloor)
                    {
                        _intent.store(
                            static_cast<uint8_t>(PlaybackIntent::Playing),
                            std::memory_order_release);
                    }
                }
                _videoQueue.push(
                    frame,
                    _seekAppliedSerial.load(std::memory_order_relaxed));
                continue;
            }
            if (fr == VideoResult::EndOfStream)
            {
                eosSeen = true;
                return false;
            }
            if (fr == VideoResult::DecodeError)
            {
                // V4 soft-skip: drop a bad frame and keep draining.
                _skippedErrors.fetch_add(1);
                continue;
            }
            if (fr != VideoResult::Ok)
            {
                _failure.store(fr);
                return false;
            }
            // Ok + null video: drain any pending audio, then yield.
            // ScrubPreview is preview-only — discard PCM so play-after-
            // scrub does not flush a keyframe→thumb backlog.
            if (_audioQueue)
            {
                const bool dropAudio =
                    intent() == PlaybackIntent::ScrubPreview;
                for (;;)
                {
                    if (_cancel.load())
                    {
                        return false;
                    }
                    const VideoResult ar = _decoder.dequeueAudioFrame(audio);
                    if (ar == VideoResult::Ok && audio.data && audio.frameCount > 0)
                    {
                        if (!dropAudio)
                        {
                            _audioQueue->push(audio);
                        }
                        continue;
                    }
                    if (ar == VideoResult::EndOfStream)
                    {
                        break;
                    }
                    if (ar != VideoResult::Ok)
                    {
                        _failure.store(ar);
                        return false;
                    }
                    break; // Ok + null
                }
            }
            return true;
        }
    };

    auto feedOne = [&](const VideoPacket& packet) -> bool {
        for (;;)
        {
            if (_cancel.load())
            {
                return false;
            }
            if (_seekSerial.load(std::memory_order_relaxed)
                != _seekAppliedSerial.load(std::memory_order_relaxed))
            {
                return true; // let outer apply seek
            }
            const VideoResult fr = _decoder.feedPacket(packet);
            if (fr == VideoResult::QueueFull)
            {
                bool eosSeen = false;
                if (!pumpFrames(eosSeen))
                {
                    if (eosSeen)
                    {
                        _endedClean.store(true);
                    }
                    return false;
                }
                continue;
            }
            if (fr == VideoResult::DecodeError)
            {
                // V4 soft-skip: drop this packet and continue demuxing.
                _skippedErrors.fetch_add(1);
                return true;
            }
            if (fr != VideoResult::Ok)
            {
                _failure.store(fr);
                return false;
            }
            bool eosSeen = false;
            if (!pumpFrames(eosSeen))
            {
                if (eosSeen)
                {
                    _endedClean.store(true);
                }
                return false;
            }
            return true;
        }
    };

    for (;;)
    {
        if (_cancel.load())
        {
            break;
        }

        if (applyPendingSeek())
        {
            reconnectUsed = 0;
            if (_seekResult.load() != VideoResult::Ok)
            {
                // Seek failed — keep the loop alive; player reads result.
                continue;
            }
            continue; // resume demux from the new position
        }

        // Ceiling pause only under ScrubPreview.
        if (intent() == PlaybackIntent::ScrubPreview
            && _scrubActive.load(std::memory_order_relaxed)
            && _scrubPausedAtCeiling.load(std::memory_order_relaxed))
        {
            if (_seekSerial.load(std::memory_order_relaxed)
                != _seekAppliedSerial.load(std::memory_order_relaxed))
            {
                continue;
            }
            // Intent may have left ScrubPreview while we slept.
            if (intent() != PlaybackIntent::ScrubPreview)
            {
                endScrubMode();
                if (_pendingOverCeiling)
                {
                    VideoFrame f = reseatFrame(_pendingOverCeiling->frame,
                                               _pendingOverCeiling->pixels);
                    _videoQueue.push(f, _pendingOverCeiling->seekSerial);
                    _pendingOverCeiling.reset();
                }
                bool eosSeen = false;
                if (!pumpFrames(eosSeen))
                {
                    if (eosSeen)
                    {
                        _endedClean.store(true);
                    }
                    break;
                }
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!_scrubPausedAtCeiling.load(std::memory_order_relaxed))
            {
                bool eosSeen = false;
                if (!pumpFrames(eosSeen))
                {
                    if (eosSeen)
                    {
                        _endedClean.store(true);
                    }
                    break;
                }
            }
            continue;
        }

        const VideoResult r = _demuxer.readNextPacket(pkt);
        if (r == VideoResult::Ok)
        {
            reconnectUsed = 0; // healthy read resets the streak
            if (_logNextVideoPts && pkt.isVideo)
            {
                _logNextVideoPts = false;
                const double ptsSec = pkt.pts.toUs() / 1e6;
                const double targetSec =
                    _seekTargetUs.load(std::memory_order_relaxed) / 1e6;
                // Allow a little slack for B-frame reorder; reject anything
                // clearly from before this seek's demux land.
                const std::int64_t minPts =
                    pkt.pts.toUs() > 100'000 ? pkt.pts.toUs() - 100'000 : 0;
                _postSeekMinPtsUs.store(minPts, std::memory_order_release);
                if (_scrubActive.load(std::memory_order_relaxed))
                {
                    _scrubLandPtsUs.store(pkt.pts.toUs(),
                                          std::memory_order_release);
                }
                std::fprintf(stderr,
                             "[AYVideo][seek] decode.firstPktAfterSeek "
                             "pts=%.3fs target=%.3fs minEnqueue=%.3fs%s\n",
                             ptsSec, targetSec, minPts / 1e6,
                             (targetSec > 1.0 && ptsSec < 0.25)
                                 ? "  << LAND_NEAR_ZERO"
                                 : "");
                std::fflush(stderr);
            }
            if (pkt.isSubtitle)
            {
                ingestSubtitlePacket(_subtitleCues, pkt);
                continue;
            }
            if (!feedOne(pkt))
            {
                break;
            }
            continue;
        }

        // Seek may have interrupted a blocking read via requestAbort —
        // if a seek is pending, prefer applying it over treating abort
        // as a hard demux failure.
        if (applyPendingSeek())
        {
            reconnectUsed = 0;
            continue;
        }

        if (r == VideoResult::EndOfStream)
        {
            VideoPacket end{};
            end.isVideo = true;
            (void)_decoder.feedPacket(end);
            if (_audioQueue)
            {
                VideoPacket aend{};
                aend.isVideo = false;
                (void)_decoder.feedPacket(aend);
            }
            bool eosSeen = false;
            if (!pumpFrames(eosSeen) && eosSeen)
            {
                _endedClean.store(true);
                break;
            }
            if (_decoder.flush() != VideoResult::Ok)
            {
                break;
            }
            eosSeen = false;
            pumpFrames(eosSeen);
            if (applyPendingSeek())
            {
                // Seek after EOS (loop / scrub) — keep running.
                _endedClean.store(false);
                reconnectUsed = 0;
                continue;
            }
            _endedClean.store(true);
            break;
        }

        if (r == VideoResult::DemuxError)
        {
            if (_options.reconnectMax > 0)
            {
                if (reconnectUsed >= _options.reconnectMax)
                {
                    _failure.store(VideoResult::DemuxError);
                    break;
                }
                const uint32_t delayMs =
                    _options.reconnectDelayMs > 0 ? _options.reconnectDelayMs
                                                  : 1u;
                // Short sleep so tests stay fast; cancel can abort early.
                for (uint32_t slept = 0; slept < delayMs; slept += 5)
                {
                    if (_cancel.load())
                    {
                        break;
                    }
                    if (_seekSerial.load(std::memory_order_relaxed)
                        != _seekAppliedSerial.load(std::memory_order_relaxed))
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (_cancel.load())
                {
                    break;
                }
                if (applyPendingSeek())
                {
                    reconnectUsed = 0;
                    continue;
                }
                ++reconnectUsed;
                _reconnectAttempts.fetch_add(1);
                if (_demuxer.reconnect() == VideoResult::Ok)
                {
                    (void)_decoder.flush();
                    continue;
                }
                // Failed reopen — count against max and retry / fail.
                continue;
            }
            // V4 soft-skip: mid-stream demux glitch — continue reading.
            _skippedErrors.fetch_add(1);
            continue;
        }

        _failure.store(r);
        break;
    }

    _running.store(false);
    _finished.store(true);
}

} // namespace ayt::video
