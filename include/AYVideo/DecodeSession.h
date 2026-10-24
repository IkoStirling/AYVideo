#pragma once
#include <AYVideo/IVideoDemuxer.h>
#include <AYVideo/VideoAudioFrame.h>
#include <memory>
#include <vector>
namespace ayt::video {
struct DecodedSample {
    /// Owned top-down BGRA8 pixels; empty for an audio-only decoded sample.
    std::vector<uint8_t> bgra;
    int32_t width=0,height=0;
    ayt::time::Duration videoPts{};
    std::vector<float> audio;
    uint32_t sampleRate=0;uint16_t channels=0;
    ayt::time::Duration audioPts{};
};
/// CPU media decode without World/Scene/Renderer/AudioEngine dependencies.
/// One owner thread drives all calls except cooperative requestAbort(). Samples
/// own their buffers; the caller supplies presentation pacing and PCM output.
class DecodeSession {
public:
    DecodeSession();~DecodeSession();
    DecodeSession(const DecodeSession&)=delete;
    DecodeSession& operator=(const DecodeSession&)=delete;
    /// Reset/open the explicit source. Video is required; audio decoding is optional.
    VideoResult open(const DemuxerOpenParams& source,bool audio=true);
    /// Return owned sample data, empty Ok progress, EndOfStream or an explicit error.
    VideoResult next(DecodedSample& sample);
    /// Owner-only seek clears cancellation; host must discard old presentation queues.
    VideoResult seek(ayt::time::Duration position);
    /// Copy metadata on the session owner thread.
    MediaInfo info()const;
    /// The only cross-thread method: cooperative interrupt, not a synchronous close.
    void requestAbort()noexcept;
    /// Owner-only acknowledgement before resuming reads.
    void clearAbort()noexcept;
    /// Owner-only idempotent teardown after any external worker has joined.
    void close()noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
