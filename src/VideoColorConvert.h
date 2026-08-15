#pragma once
// VideoColorConvert.h — CPU YUV→RGBA8 (design.md §12).
//
// Pure C++ BT.601 limited-range conversion so unit tests do not need
// libswscale. FFmpeg backends may later swap to swscale PRIVATEly
// without changing this public helper signature.

#include <AYVideo/VideoFrame.h>
#include <AYVideo/VideoTypes.h>

#include <cstdint>
#include <vector>

namespace ayt::video
{

// Writes tightly packed RGBA8 into `out` (resized to w*h*4).
// Supports I420, NV12, RGBA8 (copy), BGRA8 (swizzle).
VideoResult convertFrameToRgba8(const VideoFrame& frame,
                                std::vector<uint8_t>& out);

} // namespace ayt::video
