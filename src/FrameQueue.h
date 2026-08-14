#pragma once
// FrameQueue.h — SPSC frame ring (design.md §6.3, normative for V1).
//
// Single-producer (decode thread) / single-consumer (player thread)
// ring buffer. Elements own their pixel bytes: QueuedFrame.frame.data
// points into QueuedFrame.pixels — safe to move by value, never copy.
//
// Contract (§6.3):
//   * push()   — producer side, BLOCKING backpressure when full
//                (queue depth 4, §16.3); never fails.
//   * tryPop() — consumer side, non-blocking; false = empty, which maps
//                to the §6.2 "Ok + null data" semantics. The queue never
//                reports EOS — EOS only comes from the decoder flush.
//   * clear()  — consumer side only: drops all queued frames (seek/stop
//                flush sequence, §8.3). Safe to call while the producer
//                is blocked in push().
//
// Lock-free via per-slot sequence counters (Vyukov-style SPSC ring);
// the producer spins with yield while full (backpressure). The
// single-producer/single-consumer requirement is a contract, not a
// runtime check (design.md §4.4).

#include <AYVideoFrame.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace ayt::video
{

struct QueuedFrame
{
    VideoFrame frame;
    std::vector<uint8_t> pixels;   // owns the pixel bytes

    QueuedFrame() = default;
    QueuedFrame(const QueuedFrame&) = delete;
    QueuedFrame& operator=(const QueuedFrame&) = delete;
    QueuedFrame(QueuedFrame&& other) noexcept
        : frame(other.frame)
        , pixels(std::move(other.pixels))
    {
        // Re-seat data after the vector move — a memcpy'd VideoFrame
        // still held the old pixels.data() pointer.
        if (!pixels.empty())
        {
            frame.data = pixels.data();
        }
        other.frame = VideoFrame{};
    }
    QueuedFrame& operator=(QueuedFrame&& other) noexcept
    {
        if (this != &other)
        {
            frame = other.frame;
            pixels = std::move(other.pixels);
            if (!pixels.empty())
            {
                frame.data = pixels.data();
            }
            else
            {
                frame.data = nullptr;
            }
            other.frame = VideoFrame{};
        }
        return *this;
    }
};

class FrameQueue
{
public:
    explicit FrameQueue(uint32_t depth = 4)
    {
        // Round up to the next power of two (mask-based indexing).
        uint32_t cap = 1;
        while (cap < depth)
        {
            cap <<= 1;
        }
        _capacity = cap;
        _mask = cap - 1;
        // unique_ptr<Slot[]> — Slot holds atomics (non-movable), so it
        // cannot live in std::vector (MSVC resize requires move/copy).
        _slots = std::make_unique<Slot[]>(cap);
        for (uint32_t i = 0; i < cap; ++i)
        {
            _slots[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // Producer: copies the frame payload into the ring. Blocks while
    // full (backpressure). Must not be called from the consumer thread.
    // Null-payload frames are dropped (defensive; the decode loop only
    // pushes frames with data — §6.2).
    void push(const VideoFrame& frame)
    {
        if (frame.data == nullptr || frame.dataSize == 0)
        {
            return;
        }
        // data..data+dataSize is always a contiguous span (FFmpegDecoder
        // packs planes; Mock emits a single RGBA buffer).
        constexpr uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;
        if (frame.dataSize > kMaxFrameBytes)
        {
            return;
        }
        const uint64_t head = _head.load(std::memory_order_relaxed);
        Slot& slot = _slots[head & _mask];
        while (slot.sequence.load(std::memory_order_acquire) != head)
        {
            std::this_thread::yield();
        }

        slot.data.pixels.assign(frame.data, frame.data + frame.dataSize);
        slot.data.frame = frame;
        slot.data.frame.data = slot.data.pixels.data();

        slot.sequence.store(head + 1, std::memory_order_release);
        _head.store(head + 1, std::memory_order_relaxed);
    }

    // Consumer: moves the head frame into `out`. Returns false when
    // empty (no frame ready — §6.2 semantics).
    bool tryPop(QueuedFrame& out)
    {
        const uint64_t tail = _tail.load(std::memory_order_relaxed);
        Slot& slot = _slots[tail & _mask];
        if (slot.sequence.load(std::memory_order_acquire) != tail + 1)
        {
            return false; // empty
        }
        out = std::move(slot.data);
        slot.sequence.store(tail + _capacity, std::memory_order_release);
        _tail.store(tail + 1, std::memory_order_relaxed);
        return true;
    }

    // Consumer: drop all queued frames. Safe while the producer is
    // blocked in push() — the blocked push completes into an emptied
    // ring, which is exactly the desired unblock (flush sequence §8.3).
    //
    // Slot i's next absolute write index after the jump is
    // head + ((i - head) mod capacity): sequences are re-based there so
    // the producer's wait (sequence == its head) still resolves.
    void clear()
    {
        const uint64_t head = _head.load(std::memory_order_relaxed);
        _tail.store(head, std::memory_order_relaxed);
        const uint64_t base = head & _mask;
        for (uint32_t i = 0; i < _capacity; ++i)
        {
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

    uint32_t capacity() const noexcept { return _capacity; }

private:
    struct Slot
    {
        std::atomic<uint64_t> sequence{0};
        QueuedFrame data;
    };

    uint32_t _capacity = 0;
    uint32_t _mask = 0;
    std::unique_ptr<Slot[]> _slots;
    std::atomic<uint64_t> _head{0};  // producer write index
    std::atomic<uint64_t> _tail{0};  // consumer read index
};

} // namespace ayt::video
