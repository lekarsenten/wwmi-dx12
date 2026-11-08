#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "../core/cb_ring.h"

// The descriptors a patched draw reads besides the mod's maps: null SRVs for the registers it has no map for,
// and the per-draw params buffer (hue, saturation, value, brightness) from a ring, each in a CPU-only heap
// the shadow bind copies from.
class RfxPatchSrvs {
public:
    bool NullSrvs(D3D12_CPU_DESCRIPTOR_HANDLE* null_tex, D3D12_CPU_DESCRIPTOR_HANDLE* null_buf);
    D3D12_CPU_DESCRIPTOR_HANDLE ParamsSrv(float hue, float sat, float val, float brightness);

private:
    bool EnsureHeap();
    static constexpr uint32_t kSlots = 4096;
    static constexpr uint32_t kParamsBytes = 256u * 1024u; // the ring's first buffer: 1024 draws
    std::atomic<bool> made_{ false };
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    std::atomic<uint32_t> next_{ 0 };
    CbRing params_ring_;
};
extern RfxPatchSrvs g_rfx_patch_srvs;
