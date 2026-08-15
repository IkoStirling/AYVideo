#include <AYVideo/IVideoBackendFactory.h>

#include "../backend/CpuVideoFrameTexture.h"
#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"
#include "../backend/MockVideoFrameTexture.h"
#include "../backend/NullDecoder.h"
#include "../backend/NullDemuxer.h"

#ifdef AYVIDEO_HAS_FFMPEG
#include "../backend/FFmpegDecoder.h"
#include "../backend/FFmpegDemuxer.h"
#endif

#ifdef AYVIDEO_HAS_AYRENDERER
#include "../backend/RendererVideoFrameTexture.h"
#endif

namespace ayt::video
{

std::unique_ptr<IAYVideoDemuxer> makeNullDemuxer()
{
    return std::make_unique<NullDemuxer>();
}

std::unique_ptr<IAYVideoDemuxer> makeMockDemuxer(int32_t packetCount)
{
    return std::make_unique<MockDemuxer>(packetCount);
}

std::unique_ptr<IAYVideoDecoder> makeNullDecoder()
{
    return std::make_unique<NullDecoder>();
}

std::unique_ptr<IAYVideoDecoder> makeMockDecoder(int32_t frameCount)
{
    return std::make_unique<MockDecoder>(frameCount);
}

std::unique_ptr<IAYVideoDemuxer> makeFFmpegDemuxer()
{
#ifdef AYVIDEO_HAS_FFMPEG
    return std::make_unique<FFmpegDemuxer>();
#else
    return nullptr;
#endif
}

std::unique_ptr<IAYVideoDecoder> makeFFmpegDecoder()
{
#ifdef AYVIDEO_HAS_FFMPEG
    return std::make_unique<FFmpegDecoder>();
#else
    return nullptr;
#endif
}

std::unique_ptr<IVideoFrameTexture> makeCpuVideoFrameTexture()
{
    return std::make_unique<CpuVideoFrameTexture>();
}

std::unique_ptr<IVideoFrameTexture> makeMockVideoFrameTexture()
{
    return std::make_unique<MockVideoFrameTexture>();
}

std::unique_ptr<IVideoFrameTexture> makeRendererVideoFrameTexture(
    ayt::render::Renderer& renderer)
{
#ifdef AYVIDEO_HAS_AYRENDERER
    return std::make_unique<RendererVideoFrameTexture>(renderer);
#else
    (void)renderer;
    return nullptr;
#endif
}

} // namespace ayt::video
