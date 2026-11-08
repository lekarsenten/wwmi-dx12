#include "rfx_shadow_heap.h"
#include "../core/gpu_progress.h"
#include "../core/log.h"
#include "../addon/addon.h"

RfxShadowHeap g_rfx_shadow;

bool RfxShadowHeap::Ensure(ID3D12Device* dev)
{
    if (heap_) return true;
    inc_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = 262144;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap_)))) return false;
    cap_ = d.NumDescriptors;
    return true;
}

bool RfxShadowHeap::ReserveLocked(uint32_t n)
{
    constexpr uint32_t kBlk = 4096;
    constexpr uint32_t kMax = 1000000;
    const uint64_t now = GpuStamp();
    for (size_t i = 0; i < retired_.size();)
        if (GpuPassed(retired_[i].second)) {
            retired_[i] = std::move(retired_.back());
            retired_.pop_back();
        } else
            ++i;
    for (;;) {
        const uint32_t cap = cap_;
        const uint32_t blocks = (cap + kBlk - 1) / kBlk;
        if (tag_.size() != blocks) tag_.assign(blocks, 0);
        uint32_t start = cur_;
        if (start + n > cap) start = 0;
        const uint32_t first = (start == cur_ && start % kBlk) ? start / kBlk + 1 : start / kBlk;
        const uint32_t last = (start + n - 1) / kBlk;
        bool free = start + n <= cap;
        for (uint32_t b = first; free && b <= last; ++b)
            if (tag_[b] && !GpuPassed(tag_[b])) free = false;
        if (free) {
            for (uint32_t b = start / kBlk; b <= last; ++b) tag_[b] = now;
            cur_ = start;
            return true;
        }
        uint32_t size = cap;
        do size = size >= kMax / 2 ? kMax : size * 2;
        while (size < n && size < kMax);
        if (size <= cap || size < n) return false; // at D3D12's limit and every slot in flight
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = size;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
        if (FAILED(g_addon.d3d12_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)))) return false;
        retired_.push_back({ heap_, now });
        heap_ = h;
        cap_ = size;
        cur_ = 0;
        tag_.clear();
        logger::write("RFX-SHADOW: shadow heap grown to %u descriptors (its slots were still in flight)", size);
    }
}

RfxShadowHeap::Lease RfxShadowHeap::Reserve(uint32_t n)
{
    Lease l;
    l.lock = std::unique_lock(mutex_);
    if (!Ensure(g_addon.d3d12_device) || !inc_) return l;
    if (!ReserveLocked(n)) return l;
    l.start = cur_;
    cur_ += n;
    l.cpu0 = heap_->GetCPUDescriptorHandleForHeapStart().ptr;
    l.gpu0 = heap_->GetGPUDescriptorHandleForHeapStart().ptr;
    l.inc = inc_;
    l.heap = heap_.Get();
    return l;
}

uint32_t RfxShadowHeap::Inc()
{
    if (!inc_) inc_ = g_addon.d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return inc_;
}
