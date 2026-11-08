#include "rfx_patch_srvs.h"
#include "rfx_shadow_heap.h"
#include "../addon/addon.h"

RfxPatchSrvs g_rfx_patch_srvs;

bool RfxPatchSrvs::EnsureHeap()
{
    if (made_.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> l(mutex_);
    if (heap_) return true;
    if (!g_addon.d3d12_device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = kSlots; // not shader-visible: a legal copy SOURCE
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
    if (FAILED(g_addon.d3d12_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)))) return false;
    const uint32_t inc = g_rfx_shadow.Inc();
    const SIZE_T base = h->GetCPUDescriptorHandleForHeapStart().ptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC t = {};
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    t.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    t.Texture2D.MipLevels = 1;
    g_addon.d3d12_device->CreateShaderResourceView(nullptr, &t, { base });
    D3D12_SHADER_RESOURCE_VIEW_DESC b = {};
    b.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    b.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    b.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    b.Buffer.NumElements = 1;
    g_addon.d3d12_device->CreateShaderResourceView(nullptr, &b, { base + inc });
    heap_ = h;
    made_.store(true, std::memory_order_release);
    return true;
}

bool RfxPatchSrvs::NullSrvs(D3D12_CPU_DESCRIPTOR_HANDLE* null_tex, D3D12_CPU_DESCRIPTOR_HANDLE* null_buf)
{
    if (!EnsureHeap()) return false;
    const SIZE_T base = heap_->GetCPUDescriptorHandleForHeapStart().ptr;
    if (null_tex) *null_tex = { base };
    if (null_buf) *null_buf = { base + g_rfx_shadow.Inc() };
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE RfxPatchSrvs::ParamsSrv(float hue, float sat, float val, float brightness)
{
    if (!EnsureHeap() || !params_ring_.Ensure(g_addon.d3d12_device, kParamsBytes)) return { 0 };
    uint64_t va = 0;
    ID3D12Resource* ring = nullptr;
    float* p = (float*)params_ring_.Take(&va, &ring);
    if (!p) return { 0 };
    p[0] = hue;
    p[1] = sat;
    p[2] = val;
    p[3] = brightness; // x217..w217
    const uint64_t off = va - ring->GetGPUVirtualAddress();
    D3D12_SHADER_RESOURCE_VIEW_DESC b = {};
    b.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    b.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    b.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    b.Buffer.FirstElement = off / 16;
    b.Buffer.NumElements = 1;
    const uint32_t slot = 2 + next_.fetch_add(1, std::memory_order_relaxed) % (kSlots - 2);
    const D3D12_CPU_DESCRIPTOR_HANDLE h = { heap_->GetCPUDescriptorHandleForHeapStart().ptr +
        (SIZE_T)slot * g_rfx_shadow.Inc() };
    g_addon.d3d12_device->CreateShaderResourceView(ring, &b, h);
    return h;
}
