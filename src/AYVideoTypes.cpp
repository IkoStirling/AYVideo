#include <AYVideoTypes.h>

namespace ayt::video
{

const char* toString(VideoResult result) noexcept
{
    switch (result)
    {
    case VideoResult::Ok:               return "Ok";
    case VideoResult::InvalidArgument:  return "InvalidArgument";
    case VideoResult::UnsupportedFormat:return "UnsupportedFormat";
    case VideoResult::DemuxError:       return "DemuxError";
    case VideoResult::DecodeError:      return "DecodeError";
    case VideoResult::EndOfStream:      return "EndOfStream";
    case VideoResult::InvalidState:     return "InvalidState";
    case VideoResult::InvalidHandle:    return "InvalidHandle";
    case VideoResult::NotInitialized:   return "NotInitialized";
    case VideoResult::StreamNotFound:   return "StreamNotFound";
    case VideoResult::OutOfMemory:      return "OutOfMemory";
    case VideoResult::QueueFull:        return "QueueFull";
    case VideoResult::Cancelled:        return "Cancelled";
    case VideoResult::Count:            return "Count";
    }
    return "Unknown";
}

const char* toString(VideoPixelFormat format) noexcept
{
    switch (format)
    {
    case VideoPixelFormat::Unknown: return "Unknown";
    case VideoPixelFormat::I420:    return "I420";
    case VideoPixelFormat::NV12:    return "NV12";
    case VideoPixelFormat::RGBA8:   return "RGBA8";
    case VideoPixelFormat::BGRA8:   return "BGRA8";
    case VideoPixelFormat::Count:   return "Count";
    }
    return "Unknown";
}

const char* toString(VideoDecodeAccel accel) noexcept
{
    switch (accel)
    {
    case VideoDecodeAccel::None:         return "None";
    case VideoDecodeAccel::Auto:         return "Auto";
    case VideoDecodeAccel::D3D11VA:      return "D3D11VA";
    case VideoDecodeAccel::DXVA2:        return "DXVA2";
    case VideoDecodeAccel::CUDA:         return "CUDA";
    case VideoDecodeAccel::VideoToolbox: return "VideoToolbox";
    case VideoDecodeAccel::VAAPI:        return "VAAPI";
    case VideoDecodeAccel::Count:        return "Count";
    }
    return "Unknown";
}

} // namespace ayt::video
