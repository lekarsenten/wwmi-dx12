#pragma once
#include "../core/d3d12_barrier.h"
#include <d3d12.h>
#include <wrl.h>
#include <unordered_map>

struct Owned {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    struct Rec {
        uint64_t gen = 0;
        bool seen = false;
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
    };
    std::unordered_map<ID3D12GraphicsCommandList*, Rec> per_list;
    uint64_t va() const { return res ? res->GetGPUVirtualAddress() : 0; }
};

// Records `o` -> `to` on `cl` (recording `gen`), from the state it is in there.
inline void Xit(ID3D12GraphicsCommandList* cl, uint64_t gen, Owned& o, D3D12_RESOURCE_STATES to)
{
    Owned::Rec& r = o.per_list[cl];
    if (!r.seen || r.gen != gen) {
        r.seen = true;
        r.gen = gen;
        r.st = D3D12_RESOURCE_STATE_COMMON;
    }
    if (r.st == to) return;
    TransitionBarrier(cl, o.res.Get(), r.st, to);
    r.st = to;
}
