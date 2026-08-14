#pragma once
// AudioQueue.h — SPSC PCM ring (design.md §11 / §16.3).
//
// Mirrors FrameQueue: producer = decode thread, consumer = player.
// Depth defaults to enough ~0.5 s chunks at typical AAC frame sizes
// (capacity is slot count, not sample count). Overflow drops the oldest
// queued chunk (audio latency is more harmful than a brief gap).

#include <AYVideoAudioFrame.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace ayt::video
{

struct QueuedAudio
{
    AudioPcmFrame frame;
    std::vector<float> samples;

    QueuedAudio() = default;
    QueuedAudio(const QueuedAudio&) = delete;
    QueuedAudio& operator=(const QueuedAudio&) = delete;
    QueuedAudio(QueuedAudio&& other) noexcept
        : frame(other.frame)
        , samples(std::move(other.samples))
    {
        if (!samples.empty())
        {
            frame.data = samples.data();
        }
        other.frame = AudioPcmFrame{};
    }
    QueuedAudio& operator=(QueuedAudio&& other) noexcept
    {
        if (this != &other)
        {
            frame = other.frame;
            samples = std::move(other.samples);
            frame.data = samples.empty() ? nullptr : samples.data();
            other.frame = AudioPcmFrame{};
        }
        return *this;
    }
};

class AudioQueue
{
public:
    // Slot count (power-of-two). Default 32 chunks ≈ several hundred ms
    // of AAC frames; overflow drops oldest (design.md §11).
    explicit AudioQueue(uint32_t depth = 32)
    {
        uint32_t cap = 1;
        while (cap < depth)
        {
            cap <<= 1;
        }
        _capacity = cap;
        _mask = cap - 1;
        _slots = std::make_unique<Slot[]>(cap);
        for (uint32_t i = 0; i < cap; ++i)
        {
            _slots[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // Producer: copy PCM into the ring. If full, drop the oldest slot
    // (advance tail) then write — never block (audio latency priority).
    void push(const AudioPcmFrame& frame)
    {
        if (frame.data == nullptr || frame.frameCount == 0 || frame.channels == 0)
        {
            return;
        }
        const uint32_t sampleCount = frame.frameCount * frame.channels;
        constexpr uint32_t kMaxSamples = 48000u * 2u; // 1 s stereo defensive
        if (sampleCount == 0 || sampleCount > kMaxSamples)
        {
            return;
        }

        uint64_t head = _head.load(std::memory_order_relaxed);
        Slot& slot = _slots[head & _mask];
        if (slot.sequence.load(std::memory_order_acquire) != head)
        {
            // Full: drop oldest.
            const uint64_t tail = _tail.load(std::memory_order_relaxed);
            Slot& old = _slots[tail & _mask];
            if (old.sequence.load(std::memory_order_acquire) == tail + 1)
            {
                old.data = QueuedAudio{};
                old.sequence.store(tail + _capacity, std::memory_order_release);
                _tail.store(tail + 1, std::memory_order_relaxed);
            }
            else
            {
                return; // race; skip this push
            }
        }

        // Re-load after possible drop.
        head = _head.load(std::memory_order_relaxed);
        Slot& dest = _slots[head & _mask];
        while (dest.sequence.load(std::memory_order_acquire) != head)
        {
            // Still full after one drop — give up (rare).
            return;
        }

        dest.data.samples.assign(frame.data, frame.data + sampleCount);
        dest.data.frame = frame;
        dest.data.frame.data = dest.data.samples.data();
        dest.sequence.store(head + 1, std::memory_order_release);
        _head.store(head + 1, std::memory_order_relaxed);
    }

    bool tryPop(QueuedAudio& out)
    {
        const uint64_t tail = _tail.load(std::memory_order_relaxed);
        Slot& slot = _slots[tail & _mask];
        if (slot.sequence.load(std::memory_order_acquire) != tail + 1)
        {
            return false;
        }
        out = std::move(slot.data);
        slot.sequence.store(tail + _capacity, std::memory_order_release);
        _tail.store(tail + 1, std::memory_order_relaxed);
        return true;
    }

    void clear()
    {
        const uint64_t head = _head.load(std::memory_order_relaxed);
        _tail.store(head, std::memory_order_relaxed);
        const uint64_t base = head & _mask;
        for (uint32_t i = 0; i < _capacity; ++i)
        {
            _slots[i].data = QueuedAudio{};
            _slots[i].sequence.store(
                head + (((uint64_t)i + _capacity - base) & _mask),
                std::memory_order_relaxed);
        }
    }

    bool empty() const noexcept
    {
        return _tail.load(std::memory_order_relaxed)
               == _head.load(std::memory_order_relaxed);
    }

private:
    struct Slot
    {
        std::atomic<uint64_t> sequence{0};
        QueuedAudio data;
    };

    uint32_t _capacity = 0;
    uint32_t _mask = 0;
    std::unique_ptr<Slot[]> _slots;
    std::atomic<uint64_t> _head{0};
    std::atomic<uint64_t> _tail{0};
};

} // namespace ayt::video
