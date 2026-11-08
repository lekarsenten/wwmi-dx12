// The readback: a texture whose bytes never passed an upload hook, hashed from a GPU copy. See tex_readback.h.
#include "tex_readback.h"
#include "tex_upload_identity.h"
#include "tex_mip_identity.h"
#include "tex_toggles.h"
#include "../hooks/command_list_registry.h" // CommandListRegistry::Gen / t_list_resets -- when a readback's list reached its queue
#include "../instrumentation/diag_gate.h"
#include "../core/d3d12_buffer.h"
#include "../core/log.h"
#include "../migoto/port_api.h"
#include <algorithm>
#include <chrono>

using namespace reshade::api;
using Microsoft::WRL::ComPtr;

TexReadback g_tex_readback;

bool TexReadback::Reserve(ID3D12Device* dev, uint64_t total, bool once, Pending& p, ID3D12Resource** buf)
{
    const auto t_wait = std::chrono::steady_clock::now();
    std::lock_guard l(mtx_);
    {
        const uint64_t waited_us =
            (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_wait)
                .count();
        if (waited_us >= 2000) {
            g_tex_stats.ts_rb_waits.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> logged{ 0 };
            if (g_debug && logged.fetch_add(1) < 20)
                logger::write("TEXRB-WAIT thread %u waited %.1f ms for the readback lock to record a %llu B readback",
                    GetCurrentThreadId(), waited_us / 1000.0, (unsigned long long)total);
        }
    }
    if (once && tried_.count(p.dst)) return false; // one-shot per resource
    if (!rb_) {
        CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST, &rb_);
        if (!rb_) return false;
    }
    dev_ = dev;
    const uint32_t aligned = (cursor_ + 511u) & ~511u;
    if ((uint64_t)aligned + total <= kBytes) {
        p.off = aligned;
        cursor_ = aligned + (uint32_t)total;
    } else {
        size_t best = spare_.size();
        for (size_t i = 0; i < spare_.size(); ++i)
            if (spare_[i].bytes >= total && (best == spare_.size() || spare_[i].bytes < spare_[best].bytes)) best = i;
        if (best != spare_.size()) {
            p.own = std::move(spare_[best].buf);
            spare_[best] = std::move(spare_.back());
            spare_.pop_back();
            g_tex_stats.ts_rb_reused.fetch_add(1, std::memory_order_relaxed);
        } else {
            uint64_t bytes = 64u * 1024;
            while (bytes < total) bytes <<= 1;
            const auto t0 = std::chrono::steady_clock::now();
            CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, &p.own);
            const uint64_t us =
                (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
                    .count();
            g_tex_stats.ts_rb_own.fetch_add(1, std::memory_order_relaxed);
            g_tex_stats.ts_rb_own_us.fetch_add(us, std::memory_order_relaxed);
            static std::atomic<int> logged{ 0 };
            if (g_debug && us >= 2000 && logged.fetch_add(1) < 20)
                logger::write(
                    "TEXRB-OWN %llu B readback buffer created in %.1f ms on thread %u -- the shared one is full to %u of %u B with "
                    "%zu readback(s) pending",
                    (unsigned long long)bytes, us / 1000.0, GetCurrentThreadId(), cursor_, kBytes, pending_.size());
        }
        own_used_ms_ = GetTickCount64();
        if (!p.own) return false;
    }
    *buf = p.own ? p.own.Get() : rb_.Get();
    pending_.push_back(p);
    unsignalled_.fetch_add(1, std::memory_order_relaxed);
    if (once) tried_.insert(p.dst);
    return true;
}

void TexReadback::KeepSpare(ComPtr<ID3D12Resource> b)
{
    if (!b) return;
    const uint64_t bytes = b->GetDesc().Width;
    spare_.push_back(Spare{ std::move(b), bytes });
}

bool TexReadback::Schedule(ID3D12Device* dev, command_list* list, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
    ID3D12Resource* dst, const D3D12_RESOURCE_DESC& sd)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&sd, 0, 1, 0, &fp, &rows, &rowSize, &total);
    Pending p;
    p.dst = dst;
    p.rowPitch = fp.Footprint.RowPitch;
    p.list = list;
    p.gen = g_command_lists.Gen(list);
    ID3D12Resource* own_or_shared = nullptr;
    if (!Reserve(dev, total, true, p, &own_or_shared)) return false;
    dst->AddRef(); // keep alive until Drain hashes it
    D3D12_TEXTURE_COPY_LOCATION d = {};
    d.pResource = own_or_shared;
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = fp;
    d.PlacedFootprint.Offset = p.off;
    D3D12_TEXTURE_COPY_LOCATION s = {};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr); // src already copy-source-ready, no render pass: no barrier
    return true;
}

bool TexReadback::ScheduleUpload(ID3D12Device* dev, command_list* list, ID3D12GraphicsCommandList* cl,
    ID3D12Resource* src, uint64_t src_off, uint64_t bytes, ID3D12Resource* dst, uint32_t sub, const uint32_t* cpu_crc)
{
    Pending p;
    p.dst = dst;
    p.sub = sub;
    p.list = list;
    p.gen = g_command_lists.Gen(list);
    if (cpu_crc) {
        p.compare = true;
        p.cpu_crc = *cpu_crc;
    }
    ID3D12Resource* buf = nullptr;
    if (!Reserve(dev, bytes, false, p, &buf)) return false;
    dst->AddRef();
    cl->CopyBufferRegion(buf, p.off, src, src_off, bytes); // an upload heap is always copy-source-ready
    return true;
}

void TexReadback::Arrived(const Pending& p, const uint8_t* data)
{
    if (p.compare) {
        TexAssetLayout L;
        if (TexAssetLayoutOf(p.dst->GetDesc(), p.sub, L)) {
            const std::vector<uint8_t> tight = TightFromUpload(data, L.pitch, L);
            const uint32_t gpu_crc = PortApi::Crc32c(0, tight.data(), tight.size());
            (gpu_crc == p.cpu_crc ? g_tex_stats.ts_up_same : g_tex_stats.ts_up_diff)
                .fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> n{ 0 };
            if (gpu_crc != p.cpu_crc && n.fetch_add(1) < 20)
                logger::write("TEXUP-DIFF res=%p mip %u %ux%u: the GPU copy hashes %08x, the processor's read %08x",
                    (void*)p.dst, p.sub, L.w, L.h, gpu_crc, p.cpu_crc);
        }
    }
    if (p.sub)
        g_tex_mip_identity.FromUpload(p.dst, p.sub, data);
    else
        g_tex_upload.HashFromUpload(p.dst, data, p.rowPitch);
}

TexReadback::QueueFence* TexReadback::FenceOf(ID3D12CommandQueue* q)
{
    for (QueueFence& f : fences_)
        if (f.queue == q) return &f;
    QueueFence f;
    if (!dev_ || FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence)))) return nullptr;
    q->AddRef();
    f.queue = q;
    fences_.push_back(std::move(f));
    return &fences_.back();
}

template <class Behind> unsigned TexReadback::SignalBehind(QueueFence* qf, Behind behind)
{
    bool any = false;
    for (const Pending& p : pending_) any |= p.qf == qf && !p.value && behind(p);
    if (!any || FAILED(qf->queue->Signal(qf->fence.Get(), qf->next + 1))) return 0;
    ++qf->next;
    unsigned n = 0;
    for (Pending& p : pending_)
        if (p.qf == qf && !p.value && behind(p)) {
            p.value = qf->next;
            unsignalled_.fetch_sub(1, std::memory_order_relaxed);
            ++n;
        }
    return n;
}

void TexReadback::OnExecute(command_queue* queue, command_list* list)
{
    if (!queue || !list || unsignalled_.load(std::memory_order_relaxed) == 0) return;
    auto* const q = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    const uint32_t tid = GetCurrentThreadId();
    const uint64_t gen = g_command_lists.Gen(list);
    std::lock_guard l(mtx_);
    for (QueueFence& f : fences_)
        if (f.queue == q) SignalBehind(&f, [&](const Pending& p) { return p.tid == tid && p.resets != t_list_resets; });
    for (Pending& p : pending_)
        if (!p.exec_ms && p.list == list && p.gen == gen) {
            p.qf = FenceOf(q);
            p.tid = tid;
            p.resets = t_list_resets;
            p.exec_ms = GetTickCount64();
            if (!p.qf) unsignalled_.fetch_sub(1, std::memory_order_relaxed);
        }
}

void TexReadback::Drain()
{
    if (g_debug) { // TEXSTREAM: the arrival paths, cumulative, every 600 frames
        static uint32_t tick = 0;
        if (++tick % 600 == 0)
            logger::write(
                "TEXSTREAM tex2tex=%u stream=%u streamBigUnknown=%u | buf mip0=%u mipK=%u mipKtarget=%u held=%u heldResolved=%u | targets=%u | readbackLate=%u | "
                "uploadsByGpuCopy=%u (same=%u differ=%u) | readbackOwnBuffers created=%u (%.1f ms) reused=%u lockWaits2ms=%u",
                g_tex_stats.ts_tex2tex.load(), g_tex_stats.ts_stream.load(), g_tex_stats.ts_stream_nobig.load(),
                g_tex_stats.ts_buf_mip0.load(), g_tex_stats.ts_buf_mipk.load(), g_tex_stats.ts_mipk_target.load(),
                g_tex_stats.ts_mipk_held.load(), g_tex_stats.ts_held_resolved.load(), g_tex_stats.ts_target_mip0.load(),
                g_tex_stats.ts_rb_late.load(), g_tex_stats.ts_up_gpu.load(), g_tex_stats.ts_up_same.load(),
                g_tex_stats.ts_up_diff.load(), g_tex_stats.ts_rb_own.load(), g_tex_stats.ts_rb_own_us.load() / 1000.0,
                g_tex_stats.ts_rb_reused.load(), g_tex_stats.ts_rb_waits.load());
    }
    std::vector<Pending> arrived;
    std::vector<ID3D12Resource*> dropped_dst;
    uint32_t mapped_end = 0;
    {
        std::lock_guard l(mtx_);
        const ULONGLONG now = GetTickCount64();
        for (QueueFence& f : fences_)
            g_tex_stats.ts_rb_late.fetch_add(
                SignalBehind(&f, [&](const Pending& p) { return now - p.exec_ms >= 1000; }), std::memory_order_relaxed);
        size_t keep = 0;
        for (size_t i = 0; i < pending_.size(); ++i) {
            Pending& p = pending_[i];
            const bool dropped = !p.exec_ms && g_command_lists.Gen(p.list) != p.gen;
            const bool ran =
                p.value ? p.qf->fence->GetCompletedValue() >= p.value : (p.exec_ms && !p.qf && now - p.exec_ms >= 2000);
            if (!dropped && !ran) {
                if (keep != i) pending_[keep] = std::move(p);
                ++keep;
                continue;
            }
            if (dropped) unsignalled_.fetch_sub(1, std::memory_order_relaxed);
            if (ran) {
                arrived.push_back(std::move(p));
            } else {
                dropped_dst.push_back(p.dst);
                KeepSpare(std::move(p.own));
            }
        }
        pending_.erase(pending_.begin() + keep, pending_.end());
        mapped_end = cursor_;
    }
    void* shared = nullptr;
    for (const Pending& p : arrived) {
        if (p.own) {
            void* q = nullptr;
            if (SUCCEEDED(p.own->Map(0, nullptr, &q)) && q) {
                Arrived(p, (const uint8_t*)q);
                D3D12_RANGE wr = { 0, 0 };
                p.own->Unmap(0, &wr);
            }
            continue;
        }
        if (!shared) {
            D3D12_RANGE rr = { 0, mapped_end };
            if (FAILED(rb_->Map(0, &rr, &shared))) shared = nullptr;
        }
        if (shared) Arrived(p, (const uint8_t*)shared + p.off);
    }
    if (shared) {
        D3D12_RANGE wr = { 0, 0 };
        rb_->Unmap(0, &wr);
    }
    {
        std::lock_guard l(mtx_);
        for (Pending& p : arrived) KeepSpare(std::move(p.own));
        if (!spare_.empty() && GetTickCount64() - own_used_ms_ >= 10000) spare_.clear();
        if (std::none_of(pending_.begin(), pending_.end(), [](const Pending& p) { return !p.own; })) cursor_ = 0;
    }
    for (const Pending& p : arrived) p.dst->Release();
    for (ID3D12Resource* r : dropped_dst) r->Release();
}
