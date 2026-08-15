#pragma once
// AYVideo/VideoTypes.h �?core enums + result codes for the AYVideo module.
//
// design.md §5 (V0.5): result-code discipline mirrors AYVoxel's VoxelResult
// (Ok = 0, canonical codes, `Count` sentinel appended �?never inserted
// mid-list). All public mutators return VideoResult; void failure paths
// are forbidden (design.md §18 anti-pattern table).

#include <cstdint>

namespace ayt::video
{

// ---------------------------------------------------------------------------
// Result codes. Canonical list �?append-only; never renumber, never insert
// in the middle. `Count` is the sentinel used to bound loops and to
// static_assert coverage in tests.
// ---------------------------------------------------------------------------
enum class VideoResult : uint8_t
{
    Ok = 0,
    InvalidArgument,     // bad parameters / null out-param
    UnsupportedFormat,   // container / codec / pixel format not supported
    DemuxError,          // container read / parse failure
    DecodeError,         // codec decode failure
    EndOfStream,         // demux or decoder drained
    InvalidState,        // operation illegal in current player/decoder state
    InvalidHandle,       // stale / closed stream or player reference
    NotInitialized,      // open() not called / backend not attached
    StreamNotFound,      // requested stream (video/audio) absent
    OutOfMemory,         // frame / packet allocation failed
    QueueFull,           // frame queue at capacity (backpressure signal)
    Cancelled,           // operation superseded by seek / flush / stop
    Count                // sentinel �?number of canonical codes
};

// Human-readable name for diagnostics. Every code must be covered by
// Test_VideoTypes (design.md §19).
const char* toString(VideoResult result) noexcept;

// ---------------------------------------------------------------------------
// Pixel formats for decoded video frames. V1 (FFmpeg backend) emits the
// native planar layouts below; V2's render bridge decides upload swizzle
// (design.md §8).
// ---------------------------------------------------------------------------
enum class VideoPixelFormat : uint8_t
{
    Unknown = 0,
    I420,    // YUV420 planar: Y plane, then U, then V (each 8-bit)
    NV12,    // YUV420 semi-planar: Y plane + interleaved U/V plane
    RGBA8,   // 32-bit non-premultiplied RGBA (CPU-side convenience)
    BGRA8,   // 32-bit BGRA (GPU-friendly swizzle for upload bridges)
    Count
};

const char* toString(VideoPixelFormat format) noexcept;

// V6 foresight: preferred hardware decode path. Platform support is
// fragmented � Auto tries a platform-local order then soft-falls back.
// Explicit CUDA / VideoToolbox / VAAPI may soft-fallback when unavailable
// (DecoderOpenParams::allowSoftwareFallback).
enum class VideoDecodeAccel : uint8_t
{
    None = 0,      // force software
    Auto,          // platform try-order + soft fallback
    D3D11VA,       // Windows
    DXVA2,         // Windows (legacy)
    CUDA,          // NVIDIA
    VideoToolbox,  // Apple
    VAAPI,         // Linux
    Count
};

const char* toString(VideoDecodeAccel accel) noexcept;

// V4 memory pressure: FrameQueue overflow policy (default Block).
enum class FrameQueueOverflowPolicy : uint8_t
{
    Block = 0,       // spin+yield backpressure (V1)
    DropOldest = 1,  // drop tail when full (preview / stress)
};

} // namespace ayt::video
