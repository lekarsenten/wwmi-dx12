#include "gpu_progress.h"
#include <atomic>
#include <mutex>
#include <vector>

namespace {
struct QueueFence {
    ID3D12CommandQueue* queue;
    ID3D12Fence* fence;
};
std::mutex g_mtx;
std::vector<QueueFence> g_fences;
std::atomic<uint64_t> g_signalled{ 0 };
std::atomic<uint64_t> g_done{ 0 };
} // namespace

void GpuProgressTick(ID3D12Device* dev, ID3D12CommandQueue* const* queues, unsigned n)
{
    if (!dev) return;
    std::lock_guard<std::mutex> l(g_mtx);
    for (unsigned i = 0; i < n; ++i) {
        ID3D12CommandQueue* q = queues[i];
        if (!q) continue;
        bool known = false;
        for (const QueueFence& f : g_fences) known |= (f.queue == q);
        if (known) continue;
        ID3D12Fence* f = nullptr;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) continue;
        q->AddRef();
        g_fences.push_back({ q, f });
    }
    if (g_fences.empty()) return;
    const uint64_t v = g_signalled.load(std::memory_order_relaxed) + 1;
    bool all = true;
    for (const QueueFence& f : g_fences) all &= SUCCEEDED(f.queue->Signal(f.fence, v));
    if (all) g_signalled.store(v, std::memory_order_release);
    uint64_t done = UINT64_MAX;
    for (const QueueFence& f : g_fences) {
        const uint64_t c = f.fence->GetCompletedValue();
        if (c < done) done = c;
    }
    g_done.store(done, std::memory_order_release);
}

uint64_t GpuStamp()
{
    return g_signalled.load(std::memory_order_acquire) + 2;
}

bool GpuPassed(uint64_t stamp)
{
    return g_done.load(std::memory_order_acquire) >= stamp;
}
