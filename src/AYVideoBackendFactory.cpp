#include <IAYVideoBackendFactory.h>

#include "../backend/MockDecoder.h"
#include "../backend/MockDemuxer.h"
#include "../backend/NullDecoder.h"
#include "../backend/NullDemuxer.h"

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

} // namespace ayt::video
