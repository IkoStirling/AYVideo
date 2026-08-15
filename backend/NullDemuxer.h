#pragma once
// NullDemuxer.h — silent no-op demuxer backend (V0.5).
//
// design.md §17: Null backends MUST ship in V0.5 (skeleton contract).
// Semantics: every mutator returns Ok without doing anything;
// readNextPacket returns EndOfStream immediately. Mirrors the AYAudio
// NullBackend convention ("valid, if silent, pointer").

#include <AYVideo/IVideoDemuxer.h>

namespace ayt::video
{

class NullDemuxer final : public IAYVideoDemuxer
{
public:
    VideoResult open(const DemuxerOpenParams& params) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    VideoResult getMediaInfo(MediaInfo& outInfo) const override;
    VideoResult readNextPacket(VideoPacket& outPacket) override;
    VideoResult seek(const ayt::time::Duration& target,
                     bool keyframeOnly = true) override;

private:
    bool _open = false;
};

} // namespace ayt::video
