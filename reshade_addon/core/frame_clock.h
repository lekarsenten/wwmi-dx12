#pragma once
#include <atomic>
#include <cstdint>

// The present counter: bumped once per frame by on_present, compared wherever an age in frames matters.
inline uint32_t g_frame_id = 1;

struct FrameCounter {
    std::atomic<uint32_t> n{ 0 };
    std::atomic<uint32_t> last{ 0 };
    FrameCounter& operator++()
    {
        ++n;
        return *this;
    }
    FrameCounter& operator+=(uint32_t k)
    {
        n += k;
        return *this;
    }
    void Roll() { last.store(n.exchange(0)); }
};
