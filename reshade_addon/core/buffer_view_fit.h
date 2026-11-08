#pragma once
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "log.h"

inline uint64_t BufferViewFit(ID3D12Resource* res, uint64_t first, uint64_t num, uint32_t elem_bytes, const char* who)
{
    if (!res || !elem_bytes) return num;
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return num;
    const uint64_t cap = d.Width / elem_bytes;
    const uint64_t fit = first < cap ? cap - first : 0;
    if (num <= fit) return num;
    static std::mutex m;
    static std::unordered_map<const char*, unsigned> n;
    unsigned k;
    {
        std::lock_guard<std::mutex> l(m);
        k = ++n[who];
    }
    if (k <= 20 || k % 1000 == 0)
        logger::write(
            "VIEW-OOB #%u by %s: buffer view of %llu x %u B from element %llu over res=%p of %llu B (%llu fit)", k, who,
            (unsigned long long)num, elem_bytes, (unsigned long long)first, (void*)res, (unsigned long long)d.Width,
            (unsigned long long)fit);
    return fit;
}
