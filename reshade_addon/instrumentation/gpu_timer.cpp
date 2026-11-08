// gpu_timer.cpp -- see gpu_timer.h.
#include "../addon_common.h"
#include "gpu_timer.h"
#include "../migoto/prof_core.h" // g_prof_enabled: the brackets record nothing while the profiler is off
#include "../core/d3d12_buffer.h"
#include <cstring>

GpuTimer g_gpu_timer;
GpuTimerStats g_gpu_timer_stats;

static constexpr UINT kStamps = GpuTimer::kRing * GpuTimer::kPairs * 2;

bool GpuTimer::Ensure(ID3D12Device* dev)
{
    if (ready_.load(std::memory_order_relaxed)) return true;
    if (!dev) return false;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kStamps;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb;
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap)))) return false;
    if (FAILED(CreateD3D12Buffer(
            dev, D3D12_HEAP_TYPE_READBACK, kStamps * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST, &rb)))
        return false;
    heap_ = heap;
    rb_ = rb;
    ready_.store(true, std::memory_order_release);
    return true;
}

int GpuTimer::Begin(ID3D12GraphicsCommandList* cl, int bucket)
{
    if (!g_prof_enabled || !cl || !ready_.load(std::memory_order_acquire) || bucket < 0 || bucket >= GT_COUNT)
        return -1;
    const UINT r = region_.load(std::memory_order_acquire);
    const UINT p = next_pair_[r].fetch_add(1, std::memory_order_relaxed);
    if (p >= kPairs) {
        g_gpu_timer_stats.untimed.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    pair_bucket_[r][p] = (uint8_t)bucket;
    const UINT pair = r * kPairs + p;
    cl->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, pair * 2);
    return (int)pair;
}

void GpuTimer::End(ID3D12GraphicsCommandList* cl, int pair)
{
    if (pair < 0 || !cl || !heap_) return;
    const UINT first = (UINT)pair * 2;
    cl->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
    cl->ResolveQueryData(
        heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2, rb_.Get(), (UINT64)first * sizeof(uint64_t));
}

void GpuTimer::FrameSubmitted(ID3D12Fence* fence, uint64_t value, ID3D12CommandQueue* queue)
{
    if (!heap_ || !fence || !queue) return;
    fence_ = fence;
    if (ms_per_tick_ == 0.0) {
        UINT64 hz = 0;
        if (SUCCEEDED(queue->GetTimestampFrequency(&hz)) && hz) ms_per_tick_ = 1000.0 / (double)hz;
    }
    const UINT r = region_.load(std::memory_order_relaxed);
    region_fence_[r] = value;
    region_open_[r] = true;
    const UINT next = (r + 1) % kRing;
    if (region_open_[next]) { // kRing frames behind: its stamps are stale, not read
        region_open_[next] = false;
        ++g_gpu_timer_stats.dropped;
    }
    next_pair_[next].store(0, std::memory_order_relaxed);
    region_.store(next, std::memory_order_release);
}

void GpuTimer::PresentTick(ID3D12Device* dev)
{
    if ((!heap_ && !g_prof_enabled) || !Ensure(dev) || !fence_) return;
    const uint64_t done = fence_->GetCompletedValue();
    const UINT cur = region_.load(std::memory_order_relaxed);
    for (UINT i = 1; i < kRing; ++i) { // oldest first, so consecutive marks pair up
        const UINT r = (cur + i) % kRing;
        if (region_open_[r] && done >= region_fence_[r] + 2) ReadRegion(r);
    }
}

// Sum the region's pairs into the frame accumulators, zero it for its next use.
void GpuTimer::ReadRegion(UINT r)
{
    region_open_[r] = false;
    const UINT n = std::min<UINT>(next_pair_[r].load(std::memory_order_relaxed), kPairs);
    const size_t base = (size_t)r * kPairs * 2;
    const size_t bytes = (size_t)n * 2 * sizeof(uint64_t);
    if (!n || !rb_) return;
    D3D12_RANGE rr{ base * sizeof(uint64_t), base * sizeof(uint64_t) + bytes };
    void* p = nullptr;
    if (FAILED(rb_->Map(0, &rr, &p)) || !p) return;
    uint64_t* v = (uint64_t*)p + base;
    const double ns_per_tick = ms_per_tick_ * 1e6;
    for (UINT i = 0; i < n; ++i) {
        const uint64_t t0 = v[i * 2];
        const uint64_t t1 = v[i * 2 + 1];
        const int b = pair_bucket_[r][i];
        if (t0 && t1 && t1 >= t0 && b < GT_COUNT) {
            g_gpu_timer_stats.ns[b].fetch_add((uint64_t)((double)(t1 - t0) * ns_per_tick), std::memory_order_relaxed);
            g_gpu_timer_stats.n[b].fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::memset(v, 0, bytes);
    rb_->Unmap(0, &rr);
    g_gpu_timer_stats.frames_read.fetch_add(1, std::memory_order_relaxed);
}
