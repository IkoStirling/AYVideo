// Test_VideoTypes.cpp — V0.5 stub.
//
// Asserts the value-types from design.md §5 are well-formed: enum
// sizes, Ok=0, canonical code list, toString coverage for every code.

#include <cstdint>

#include "AYTest.h"
#include "AYVideo/VideoTypes.h"

using namespace ayt::video;

TEST_SUITE(VideoTypesSuite)

    TEST_CASE(VideoResultOkIsZero) {
        // design.md §5 normative: Ok must be 0.
        CHECK_INT_EQ(static_cast<uint8_t>(VideoResult::Ok), 0u);
    }

    TEST_CASE(VideoResultEnumIsOneByte) {
        CHECK_INT_EQ(static_cast<int>(sizeof(VideoResult)), 1);
    }

    TEST_CASE(VideoResultToStringCoversAllCodes) {
        // design.md §19 verify command: covers all 13 canonical codes.
        // Accumulate a single boolean and CHECK once at the end so the
        // check count stays flat per code.
        const VideoResult all[] = {
            VideoResult::Ok,
            VideoResult::InvalidArgument,
            VideoResult::UnsupportedFormat,
            VideoResult::DemuxError,
            VideoResult::DecodeError,
            VideoResult::EndOfStream,
            VideoResult::InvalidState,
            VideoResult::InvalidHandle,
            VideoResult::NotInitialized,
            VideoResult::StreamNotFound,
            VideoResult::OutOfMemory,
            VideoResult::QueueFull,
            VideoResult::Cancelled,
        };
        bool allOk = true;
        for (auto r : all) {
            const char* s = toString(r);
            if (s == nullptr || s[0] == '\0') {
                allOk = false;
                break;
            }
        }
        CHECK_TRUE(allOk);
    }

    TEST_CASE(VideoResultCountIsSentinel) {
        // design.md §5: Count is the sentinel — it is the value after
        // the last canonical code, and static_assert coverage in tests
        // relies on it.
        CHECK_INT_EQ(static_cast<int>(VideoResult::Count), 13);
    }

    TEST_CASE(VideoPixelFormatToStringCoversAll) {
        const VideoPixelFormat all[] = {
            VideoPixelFormat::Unknown,
            VideoPixelFormat::I420,
            VideoPixelFormat::NV12,
            VideoPixelFormat::RGBA8,
            VideoPixelFormat::BGRA8,
        };
        bool allOk = true;
        for (auto f : all) {
            const char* s = toString(f);
            if (s == nullptr || s[0] == '\0') {
                allOk = false;
                break;
            }
        }
        CHECK_TRUE(allOk);
    }

    TEST_CASE(VideoPixelFormatCountIsSentinel) {
        CHECK_INT_EQ(static_cast<int>(VideoPixelFormat::Count), 5);
    }

    TEST_CASE(VideoDecodeAccelToStringCoversAll) {
        const VideoDecodeAccel all[] = {
            VideoDecodeAccel::None,
            VideoDecodeAccel::Auto,
            VideoDecodeAccel::D3D11VA,
            VideoDecodeAccel::DXVA2,
            VideoDecodeAccel::CUDA,
            VideoDecodeAccel::VideoToolbox,
            VideoDecodeAccel::VAAPI,
        };
        bool allOk = true;
        for (auto a : all) {
            const char* s = toString(a);
            if (s == nullptr || s[0] == '\0') {
                allOk = false;
                break;
            }
        }
        CHECK_TRUE(allOk);
    }

    TEST_CASE(VideoDecodeAccelCountIsSentinel) {
        CHECK_INT_EQ(static_cast<int>(VideoDecodeAccel::Count), 7);
    }

TEST_SUITE_END
