#pragma once
// MockDecoder.h — scripted decoder backend (V0.5).
//
// design.md §17: Mock backends MUST ship in V0.5. Semantics: dequeueFrame
// returns "no frame ready yet" (Ok + null frame) until at least one
// packet has been fed, then replays `frameCount` synthetic RGBA8 frames
// (deterministic pixel pattern), then EndOfStream after flush().
// Interaction counters let tests assert the player↔decoder contract.

#include <AYVideo/IVideoDecoder.h>

#include <cstdint>
#include <vector>

namespace ayt::video
{

class MockDecoder final : public IAYVideoDecoder
{
public:
    explicit MockDecoder(int32_t frameCount);

    // Test observers (design.md §19).
    uint32_t openCount() const noexcept { return _openCount; }
    uint32_t feedCount() const noexcept { return _feedCount; }
    uint32_t dequeueCount() const noexcept { return _dequeueCount; }
    uint32_t flushCount() const noexcept { return _flushCount; }
    bool wasClosed() const noexcept { return _closed; }
    const DecoderOpenParams& lastOpenParams() const noexcept { return _params; }

    VideoDecodeAccel activeDecodeAccel() const noexcept override
    {
        return _open ? VideoDecodeAccel::None : VideoDecodeAccel::None;
    }

    // V4 soft-skip: the feed whose 0-based index equals `index` returns
    // DecodeError once, then subsequent feeds succeed.
    void failFeedAt(int32_t index) noexcept
    {
        _failFeedAt = index;
        _failFeedDone = false;
    }

    VideoResult open(const DecoderOpenParams& params) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    VideoResult feedPacket(const VideoPacket& packet) override;
    VideoResult dequeueFrame(VideoFrame& outFrame) override;
    VideoResult flush() override;

private:
    int32_t _frameCount = 0;
    int32_t _emitted = 0;
    bool _open = false;
    bool _closed = false;
    bool _flushed = false;
    bool _fedAny = false;
    int32_t _failFeedAt = -1;
    bool _failFeedDone = false;
    DecoderOpenParams _params{};

    uint32_t _openCount = 0;
    uint32_t _feedCount = 0;
    uint32_t _dequeueCount = 0;
    uint32_t _flushCount = 0;

    // Synthetic frame storage (stable address for the frame data
    // pointer contract). 320x240 RGBA8 = 307200 bytes.
    std::vector<uint8_t> _pixels;
};

} // namespace ayt::video
