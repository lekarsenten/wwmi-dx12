#pragma once
#include <cstdint>
#include <d3d12.h>

inline void TransitionBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

// How the resource tracker holds D3D12's COMMON: ReShade's resource_usage::general, not a state a barrier can name.
constexpr uint32_t kTrackedCommon = 0x80000000u;

inline bool CopySourceState(uint32_t tracked, D3D12_RESOURCE_STATES* out)
{
    if (tracked == kTrackedCommon) {
        *out = D3D12_RESOURCE_STATE_COPY_SOURCE;
        return true;
    }
    if (tracked == 0 || (tracked & 0xF0000000u)) return false;
    *out = static_cast<D3D12_RESOURCE_STATES>(tracked);
    return true;
}

template <class Copy>
void CopyAsSource(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES state, Copy copy,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    const bool need = (state != D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (need) TransitionBarrier(cl, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE, subresource);
    copy();
    if (need) TransitionBarrier(cl, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state, subresource);
}
