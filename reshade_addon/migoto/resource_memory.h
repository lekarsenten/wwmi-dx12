#pragma once
#include <cstdint>
#include <mutex>

namespace MigotoPort {

// Allocation sizes for resources created by Dx12Device.
struct ResourceAllocation {
    uint64_t bytes = 0;
    bool texture = false;
    bool tracked = false;
    bool unknown = false;
};

struct ResourceMemoryRow {
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint64_t unknown = 0;

    void Add(const ResourceAllocation& a) { ++count; bytes += a.bytes; unknown += a.unknown; }
    void Remove(const ResourceAllocation& a) { --count; bytes -= a.bytes; unknown -= a.unknown; }
    void Max(const ResourceMemoryRow& o)
    {
        if (o.count > count) count = o.count;
        if (o.bytes > bytes) bytes = o.bytes;
        if (o.unknown > unknown) unknown = o.unknown;
    }
};

struct ResourceMemorySnapshot {
    // Active buffers, active textures, retired buffers, retired textures.
    ResourceMemoryRow rows[4];
    void Max(const ResourceMemorySnapshot& o)
    {
        for (unsigned i = 0; i < 4; ++i) rows[i].Max(o.rows[i]);
    }
};

// Hold one lock while moving a resource from active to retired counters,
// so snapshots see a complete transition.
class ResourceMemoryCounters {
public:
    ResourceAllocation Created(uint64_t bytes, bool texture)
    {
        ResourceAllocation a{ bytes == UINT64_MAX ? 0 : bytes, texture, true, bytes == UINT64_MAX };
        std::lock_guard<std::mutex> lock(mutex_);
        memory_.rows[texture ? 1 : 0].Add(a);
        return a;
    }
    void Retired(const ResourceAllocation& a)
    {
        if (!a.tracked) return;
        std::lock_guard<std::mutex> lock(mutex_);
        memory_.rows[a.texture ? 1 : 0].Remove(a);
        memory_.rows[a.texture ? 3 : 2].Add(a);
    }
    void Released(const ResourceAllocation& a)
    {
        if (!a.tracked) return;
        std::lock_guard<std::mutex> lock(mutex_);
        memory_.rows[a.texture ? 3 : 2].Remove(a);
    }
    ResourceMemorySnapshot Snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return memory_;
    }
private:
    std::mutex mutex_;
    ResourceMemorySnapshot memory_;
};

inline ResourceMemoryCounters& ResourceMemory()
{
    // Keep counters alive for wrappers released during static teardown.
    static auto* counters = new ResourceMemoryCounters;
    return *counters;
}

} // namespace MigotoPort
