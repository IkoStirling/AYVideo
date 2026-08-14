#pragma once
// SeekLog.h — lightweight seek timing to stderr (diagnose scrub stalls).
//
// Enable full traces: set env AYVIDEO_SEEK_LOG=1
// Slow stages (>=8ms) always print even when the env is unset.

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ayt::video::seeklog
{

inline bool enabled() noexcept
{
    static int cached = -1;
    if (cached < 0)
    {
#if defined(_MSC_VER)
        char* v = nullptr;
        size_t len = 0;
        if (_dupenv_s(&v, &len, "AYVIDEO_SEEK_LOG") == 0 && v
            && v[0] != '\0' && v[0] != '0')
        {
            cached = 1;
        }
        else
        {
            cached = 0;
        }
        free(v);
#else
        const char* v = std::getenv("AYVIDEO_SEEK_LOG");
        cached = (v && v[0] != '\0' && v[0] != '0') ? 1 : 0;
#endif
    }
    return cached == 1;
}

inline double msSince(
    const std::chrono::steady_clock::time_point& t0) noexcept
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

// Always print when `ms` exceeds the slow threshold; otherwise only if
// AYVIDEO_SEEK_LOG=1.
inline void stage(const char* name, double ms) noexcept
{
    constexpr double kSlowMs = 8.0;
    if (!enabled() && ms < kSlowMs)
    {
        return;
    }
    std::fprintf(stderr, "[AYVideo][seek] %-22s %7.2f ms%s\n", name, ms,
                 ms >= kSlowMs ? "  << SLOW" : "");
    std::fflush(stderr);
}

inline void event(const char* msg) noexcept
{
    if (!enabled())
    {
        return;
    }
    std::fprintf(stderr, "[AYVideo][seek] %s\n", msg);
    std::fflush(stderr);
}

} // namespace ayt::video::seeklog
