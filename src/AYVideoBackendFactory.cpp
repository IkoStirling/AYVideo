#include <IAYVideoBackendFactory.h>

#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"
#include "../backend/NullDecoder.h"
#include "../backend/NullDemuxer.h"

#ifdef AYVIDEO_HAS_FFMPEG
#include "../backend/FFmpegDecoder.h"
#include "../backend/FFmpegDemuxer.h"
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
    // Build without the vcpkg ffmpeg package: the FFmpeg backends do not
    // exist in this binary. The player reports UnsupportedFormat on open
    // (null backends are guarded in AYVideoPlayer::open).
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

} // namespace ayt::video
