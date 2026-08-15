#pragma once
// AYVideo/VideoAudioFrame.h — decoded PCM carrier (V2, design.md §11).
//
// Interleaved float32 samples owned by the decoder until the next
// dequeueAudioFrame / flush / close. AudioQueue copies the payload so
// the player thread can push into AYAudio without racing the decode
// thread.

#include <AYTime/Duration.h>

#include <cstdint>

namespace ayt::video
{

struct AudioPcmFrame
{
    const float* data = nullptr;   // interleaved F32
    uint32_t frameCount = 0;       // samples per channel
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    ayt::time::Duration pts{};
};

} // namespace ayt::video
