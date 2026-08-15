#pragma once
// NullDecoder.h — silent no-op decoder backend (V0.5).
//
// design.md §17: Null backends MUST ship in V0.5. Semantics: open() is a
// no-op Ok; dequeueFrame() reports the "no frame ready yet" contract
// state (Ok + null frame); EndOfStream only after flush().

#include <AYVideo/IVideoDecoder.h>

namespace ayt::video
{

class NullDecoder final : public IAYVideoDecoder
{
public:
    VideoResult open(const DecoderOpenParams& params) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    VideoResult feedPacket(const VideoPacket& packet) override;
    VideoResult dequeueFrame(VideoFrame& outFrame) override;
    VideoResult flush() override;

private:
    bool _open = false;
    bool _flushed = false;
};

} // namespace ayt::video
