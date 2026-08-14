#include "VideoColorConvert.h"

#include <algorithm>
#include <cmath>

namespace ayt::video
{

namespace
{

inline uint8_t clampU8(int v) noexcept
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

// BT.601 limited-range YUV → RGB.
inline void yuvToRgba(uint8_t y, uint8_t u, uint8_t v,
                      uint8_t* rgba) noexcept
{
    const int c = static_cast<int>(y) - 16;
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;
    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;
    rgba[0] = clampU8(r);
    rgba[1] = clampU8(g);
    rgba[2] = clampU8(b);
    rgba[3] = 255;
}

} // namespace

VideoResult convertFrameToRgba8(const VideoFrame& frame,
                                std::vector<uint8_t>& out)
{
    if (frame.data == nullptr || frame.dataSize == 0
        || frame.width <= 0 || frame.height <= 0)
    {
        return VideoResult::InvalidArgument;
    }

    const uint32_t w = static_cast<uint32_t>(frame.width);
    const uint32_t h = static_cast<uint32_t>(frame.height);
    const size_t rgbaBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

    if (frame.format == VideoPixelFormat::RGBA8)
    {
        if (frame.dataSize < rgbaBytes)
        {
            return VideoResult::InvalidArgument;
        }
        out.assign(frame.data, frame.data + rgbaBytes);
        return VideoResult::Ok;
    }

    if (frame.format == VideoPixelFormat::BGRA8)
    {
        if (frame.dataSize < rgbaBytes)
        {
            return VideoResult::InvalidArgument;
        }
        out.resize(rgbaBytes);
        for (size_t i = 0; i < rgbaBytes; i += 4)
        {
            out[i + 0] = frame.data[i + 2];
            out[i + 1] = frame.data[i + 1];
            out[i + 2] = frame.data[i + 0];
            out[i + 3] = frame.data[i + 3];
        }
        return VideoResult::Ok;
    }

    if (frame.format == VideoPixelFormat::I420)
    {
        const size_t yBytes = static_cast<size_t>(w) * h;
        const uint32_t cw = (w + 1u) / 2u;
        const uint32_t ch = (h + 1u) / 2u;
        const size_t uBytes = static_cast<size_t>(cw) * ch;
        const size_t need = yBytes + uBytes + uBytes;
        if (frame.dataSize < need)
        {
            return VideoResult::InvalidArgument;
        }
        const uint8_t* yPlane = frame.data + frame.planeOffset[0];
        const uint8_t* uPlane = frame.data + frame.planeOffset[1];
        const uint8_t* vPlane = frame.data + frame.planeOffset[2];
        // Defensive: if offsets were left 0/0/0, assume tight packing.
        if (frame.planeOffset[1] == 0 && frame.planeOffset[2] == 0)
        {
            uPlane = frame.data + yBytes;
            vPlane = uPlane + uBytes;
        }

        out.resize(rgbaBytes);
        for (uint32_t row = 0; row < h; ++row)
        {
            for (uint32_t col = 0; col < w; ++col)
            {
                const uint32_t uvIndex = (row / 2u) * cw + (col / 2u);
                yuvToRgba(yPlane[row * w + col],
                          uPlane[uvIndex],
                          vPlane[uvIndex],
                          &out[(static_cast<size_t>(row) * w + col) * 4u]);
            }
        }
        return VideoResult::Ok;
    }

    if (frame.format == VideoPixelFormat::NV12)
    {
        const size_t yBytes = static_cast<size_t>(w) * h;
        const uint32_t ch = (h + 1u) / 2u;
        const size_t uvBytes = static_cast<size_t>(w) * ch;
        if (frame.dataSize < yBytes + uvBytes)
        {
            return VideoResult::InvalidArgument;
        }
        const uint8_t* yPlane = frame.data + frame.planeOffset[0];
        const uint8_t* uvPlane = frame.data + frame.planeOffset[1];
        if (frame.planeOffset[1] == 0)
        {
            uvPlane = frame.data + yBytes;
        }

        out.resize(rgbaBytes);
        for (uint32_t row = 0; row < h; ++row)
        {
            for (uint32_t col = 0; col < w; ++col)
            {
                const uint32_t uvRow = row / 2u;
                const uint32_t uvCol = col & ~1u;
                const size_t uvOff =
                    static_cast<size_t>(uvRow) * w + uvCol;
                yuvToRgba(yPlane[row * w + col],
                          uvPlane[uvOff + 0],
                          uvPlane[uvOff + 1],
                          &out[(static_cast<size_t>(row) * w + col) * 4u]);
            }
        }
        return VideoResult::Ok;
    }

    return VideoResult::UnsupportedFormat;
}

} // namespace ayt::video
