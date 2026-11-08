#include "../addon_common.h"
#include "hot_path.h"
#include "gpu_timer.h"
#include "../core/d3d12_buffer.h"
#include <TraceLoggingProvider.h>
#include <d3d12.h>
#include <wrl.h>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include "../addon/addon.h"

TRACELOGGING_DECLARE_PROVIDER(g_hp_etw);

namespace HotPath {

namespace {

enum StampKind : uint32_t {
    SK_ListBegin = 1,
    SK_ListEnd = 2,
    SK_Barrier = 3,
    SK_OursBegin = 4,
    SK_OursEnd = 5,
};

constexpr UINT kSlabStamps = 4096;
constexpr UINT kListChunk = 64;

struct Slab {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb;
    int ty = 0;
};

struct Meta {
    uint32_t kind;
    uint32_t aux;
    uint32_t works;
    int64_t cpu;
};

struct Chunk {
    Slab* slab = nullptr;
    UINT base = 0;
    UINT cap = 0;
    std::vector<Meta> meta;
    uint64_t list = 0;
    uint32_t tid = 0;
    GpuRec* owner = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    bool resolved = false;
    bool abandoned = false;
    bool read = false;
};

struct Region {
    uint64_t fence = 0;
    std::vector<std::unique_ptr<Chunk>> chunks;
    std::vector<Slab*> slabs;
    Slab* cur[2] = {};
    UINT used[2] = {};
};

std::mutex g_gmtx;
std::vector<std::unique_ptr<Slab>> g_all_slabs;
std::vector<Slab*> g_free[2];
std::unique_ptr<Region> g_open;
std::deque<std::unique_ptr<Region>> g_closed;
Microsoft::WRL::ComPtr<ID3D12Fence> g_fence;
ID3D12CommandQueue* g_present_queue = nullptr;
int g_copy_ok = -1;

int TypeOf(D3D12_COMMAND_LIST_TYPE t)
{
    if (t == D3D12_COMMAND_LIST_TYPE_DIRECT || t == D3D12_COMMAND_LIST_TYPE_COMPUTE) return 0;
    if (t == D3D12_COMMAND_LIST_TYPE_COPY) return 1;
    return -1;
}

int64_t Qpc()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

Slab* TakeSlabLocked(int ty)
{
    if (!g_free[ty].empty()) {
        Slab* s = g_free[ty].back();
        g_free[ty].pop_back();
        return s;
    }
    ID3D12Device* dev = g_addon.d3d12_device;
    if (!dev) return nullptr;
    auto s = std::make_unique<Slab>();
    s->ty = ty;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = ty ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kSlabStamps;
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&s->heap)))) return nullptr;
    if (FAILED(CreateD3D12Buffer(
            dev, D3D12_HEAP_TYPE_READBACK, kSlabStamps * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST, &s->rb)))
        return nullptr;
    void* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (SUCCEEDED(s->rb->Map(0, &none, &p)) && p) {
        std::memset(p, 0, kSlabStamps * sizeof(uint64_t));
        D3D12_RANGE all{ 0, kSlabStamps * sizeof(uint64_t) };
        s->rb->Unmap(0, &all);
    }
    g_all_slabs.push_back(std::move(s));
    return g_all_slabs.back().get();
}

bool TypeUsableLocked(int ty)
{
    if (ty < 0) return false;
    if (ty == 0) return true;
    if (g_copy_ok < 0) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
        g_copy_ok = g_addon.d3d12_device &&
                SUCCEEDED(g_addon.d3d12_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))) &&
                o3.CopyQueueTimestampQueriesSupported
            ? 1
            : 0;
    }
    return g_copy_ok == 1;
}

Chunk* NewChunk(int ty, UINT cap)
{
    std::lock_guard l(g_gmtx);
    if (!TypeUsableLocked(ty)) return nullptr;
    if (!g_open) g_open = std::make_unique<Region>();
    Region& r = *g_open;
    if (!r.cur[ty] || r.used[ty] + cap > kSlabStamps) {
        Slab* s = TakeSlabLocked(ty);
        if (!s) return nullptr;
        r.slabs.push_back(s);
        r.cur[ty] = s;
        r.used[ty] = 0;
    }
    auto c = std::make_unique<Chunk>();
    c->slab = r.cur[ty];
    c->base = r.used[ty];
    c->cap = cap;
    c->meta.reserve(cap);
    r.used[ty] += cap;
    r.chunks.push_back(std::move(c));
    return r.chunks.back().get();
}

} // namespace

struct GpuRec {
    uint64_t key = 0;
    ID3D12GraphicsCommandList* native = nullptr;
    int ty = -1;
    bool live = false;
    bool begin_pending = false;
    uint32_t works = 0;
    Chunk* cur = nullptr;
    std::vector<Chunk*> chunks;
    std::atomic<ID3D12CommandQueue*> queue{ nullptr };
};

namespace {

GpuRec g_own;

void Stamp(GpuRec* g, uint32_t kind, uint32_t aux)
{
    Chunk* c = g->cur;
    if (!c || c->meta.size() >= c->cap) {
        c = NewChunk(g->ty, kListChunk);
        if (!c) {
            g->live = false;
            return;
        }
        c->list = g->key;
        c->tid = GetCurrentThreadId();
        c->owner = g;
        g->cur = c;
        g->chunks.push_back(c);
    }
    g->native->EndQuery(c->slab->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, c->base + (UINT)c->meta.size());
    c->meta.push_back(Meta{ kind, aux, g->works, Qpc() });
    g->works = 0;
}

void ResolveAll(GpuRec* g, ID3D12CommandQueue* queue)
{
    std::lock_guard l(g_gmtx);
    for (Chunk* c : g->chunks) {
        if (!c->meta.empty())
            g->native->ResolveQueryData(c->slab->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, c->base, (UINT)c->meta.size(),
                c->slab->rb.Get(), (UINT64)c->base * sizeof(uint64_t));
        c->queue = queue;
        c->resolved = true;
    }
    g->chunks.clear();
    g->cur = nullptr;
}

bool Open(GpuRec* g, uint64_t key, ID3D12GraphicsCommandList* native)
{
    if (!native) return false;
    g->key = key;
    g->native = native;
    g->ty = TypeOf(native->GetType());
    {
        std::lock_guard l(g_gmtx);
        if (!TypeUsableLocked(g->ty)) return false;
    }
    g->live = true;
    return true;
}

bool IsWork(int hook)
{
    return hook == HK_draw_indexed || hook == HK_dispatch || hook == HK_copy_buffer_region ||
        hook == HK_copy_resource || hook == HK_copy_texture_region || hook == HK_copy_buffer_to_texture ||
        hook == HK_update_texture_region || hook == HK_dispatch_rays || hook == HK_build_acceleration_structure;
}

struct Calib {
    ID3D12CommandQueue* q;
    uint64_t gpu;
    uint64_t cpu;
    double qpc_per_tick;
};

} // namespace

void GpuListReset(Rec* rec)
{
    GpuRec*& g = GpuOf(rec);
    if (!g) {
        if (!g_gpu_on.load(std::memory_order_relaxed)) return;
        g = new GpuRec();
    }
    if (!g->chunks.empty()) {
        std::lock_guard l(g_gmtx);
        for (Chunk* c : g->chunks) c->abandoned = true;
    }
    g->chunks.clear();
    g->cur = nullptr;
    g->works = 0;
    g->native = nullptr;
    g->live = false;
    g->begin_pending = g_gpu_on.load(std::memory_order_relaxed);
}

void GpuHook(Rec* rec, int hook, reshade::api::command_list* cl)
{
    if (hook == HK_reset_command_list || hook == HK_close_command_list) return;
    GpuRec* g = GpuOf(rec);
    if (!g) return;
    if (g->begin_pending) {
        g->begin_pending = false;
        if (Open(g, reinterpret_cast<uint64_t>(cl), (ID3D12GraphicsCommandList*)cl->get_native()))
            Stamp(g, SK_ListBegin, 0);
    }
    if (!g->live) return;
    if (hook == HK_barrier && g_gpu_detail.load(std::memory_order_relaxed))
        Stamp(g, SK_Barrier, 0);
    else if (IsWork(hook))
        ++g->works;
}

void GpuNoteBarriers(uint32_t count)
{
    Rec* rec = t_rec;
    if (!rec) return;
    GpuRec* g = GpuOf(rec);
    if (!g || !g->live || !g->cur || g->cur->meta.empty()) return;
    Meta& m = g->cur->meta.back();
    if (m.kind == SK_Barrier) m.aux += count;
}

void GpuListClose(Rec* rec, reshade::api::command_list* cl, uint32_t claimed)
{
    GpuRec* g = GpuOf(rec);
    if (!g) return;
    if (g->begin_pending) {
        g->begin_pending = false;
        if (Open(g, reinterpret_cast<uint64_t>(cl), (ID3D12GraphicsCommandList*)cl->get_native()))
            Stamp(g, SK_ListBegin, 0);
    }
    if (!g->live) return;
    Stamp(g, SK_ListEnd, claimed);
    ResolveAll(g, nullptr);
    g->live = false;
}

void GpuExecuted(Rec* rec, reshade::api::command_queue* queue)
{
    GpuRec* g = GpuOf(rec);
    if (g && queue) g->queue.store((ID3D12CommandQueue*)queue->get_native(), std::memory_order_relaxed);
}

void* GpuBracketBegin(ID3D12GraphicsCommandList* cl, int bucket)
{
    if (!g_gpu_on.load(std::memory_order_relaxed) || !cl) return nullptr;
    if (bucket != GT_Present && bucket != GT_ClaimedDraw && !g_gpu_detail.load(std::memory_order_relaxed))
        return nullptr;
    const int ty = TypeOf(cl->GetType());
    Chunk* c = NewChunk(ty, 2);
    if (!c) return nullptr;
    GpuRec* g = t_rec ? GpuOf(t_rec) : nullptr;
    if (g && g->native == cl) {
        c->list = g->key;
        c->owner = g;
    } else if (g_own.native == cl) {
        c->list = g_own.key;
        c->owner = &g_own;
    } else {
        c->list = reinterpret_cast<uint64_t>(cl);
    }
    c->tid = GetCurrentThreadId();
    cl->EndQuery(c->slab->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, c->base);
    c->meta.push_back(Meta{ SK_OursBegin, (uint32_t)bucket, 0, Qpc() });
    return c;
}

void GpuBracketEnd(ID3D12GraphicsCommandList* cl, void* token)
{
    Chunk* c = (Chunk*)token;
    if (!c || !cl) return;
    cl->EndQuery(c->slab->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, c->base + 1);
    c->meta.push_back(Meta{ SK_OursEnd, c->meta[0].aux, 0, Qpc() });
    cl->ResolveQueryData(c->slab->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, c->base, 2, c->slab->rb.Get(),
        (UINT64)c->base * sizeof(uint64_t));
    std::lock_guard l(g_gmtx);
    c->resolved = true;
}

void GpuOwnListBegin(ID3D12GraphicsCommandList* cl)
{
    if (!g_own.chunks.empty()) {
        std::lock_guard l(g_gmtx);
        for (Chunk* c : g_own.chunks) c->abandoned = true;
    }
    g_own.chunks.clear();
    g_own.cur = nullptr;
    g_own.works = 0;
    g_own.live = false;
    if (!g_gpu_on.load(std::memory_order_relaxed) || !cl) return;
    if (Open(&g_own, reinterpret_cast<uint64_t>(cl), cl)) Stamp(&g_own, SK_ListBegin, 0);
}

void GpuOwnListEnd(ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* queue)
{
    if (!g_own.live || g_own.native != cl) return;
    Stamp(&g_own, SK_ListEnd, 0);
    ResolveAll(&g_own, queue);
    g_own.live = false;
}

void GpuFrameSubmitted(ID3D12Fence* fence, uint64_t value, ID3D12CommandQueue* queue)
{
    std::lock_guard l(g_gmtx);
    if (!g_open) return;
    g_fence = fence;
    g_present_queue = queue;
    g_open->fence = value;
    g_closed.push_back(std::move(g_open));
}

void GpuTick()
{
    std::lock_guard l(g_gmtx);
    if (!g_fence || g_closed.empty()) return;
    const uint64_t done = g_fence->GetCompletedValue();
    static const double qpf = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    std::vector<Calib> calib;
    auto convert = [&](ID3D12CommandQueue* q, uint64_t t) -> int64_t {
        for (const Calib& c : calib)
            if (c.q == q) return (int64_t)c.cpu + (int64_t)((double)((int64_t)(t - c.gpu)) * c.qpc_per_tick);
        Calib c{ q, 0, 0, 0.0 };
        UINT64 hz = 0;
        if (FAILED(q->GetTimestampFrequency(&hz)) || !hz || FAILED(q->GetClockCalibration(&c.gpu, &c.cpu))) return 0;
        c.qpc_per_tick = qpf / (double)hz;
        calib.push_back(c);
        return (int64_t)c.cpu + (int64_t)((double)((int64_t)(t - c.gpu)) * c.qpc_per_tick);
    };
    while (!g_closed.empty()) {
        Region& r = *g_closed.front();
        if (done < r.fence + 2) break;
        bool open = false;
        bool pending = false;
        for (auto& cp : r.chunks) {
            Chunk& c = *cp;
            if (c.read || c.abandoned) continue;
            if (!c.resolved) {
                open = true;
                continue;
            }
            ID3D12CommandQueue* q = c.queue;
            if (!q && c.owner) q = c.owner->queue.load(std::memory_order_relaxed);
            if (!q) q = g_present_queue;
            const size_t n = c.meta.size();
            D3D12_RANGE rr{ (SIZE_T)c.base * sizeof(uint64_t), (SIZE_T)(c.base + n) * sizeof(uint64_t) };
            void* p = nullptr;
            if (!n || !q || FAILED(c.slab->rb->Map(0, &rr, &p)) || !p) {
                c.read = true;
                continue;
            }
            uint64_t* v = (uint64_t*)p + c.base;
            if (!v[n - 1] && done < r.fence + 8) {
                D3D12_RANGE none{ 0, 0 };
                c.slab->rb->Unmap(0, &none);
                pending = true;
                continue;
            }
            const int64_t now = Qpc();
            for (size_t i = 0; i < n; ++i) {
                if (!v[i]) continue;
                const Meta& m = c.meta[i];
                TraceLoggingWrite(g_hp_etw, "GpuStamp", TraceLoggingUInt64(c.list, "list"),
                    TraceLoggingPointer(q, "queue"), TraceLoggingUInt32(c.tid, "tid"),
                    TraceLoggingUInt32(m.kind, "kind"), TraceLoggingUInt32(m.aux, "aux"),
                    TraceLoggingUInt32(m.works, "works"), TraceLoggingInt64(m.cpu, "cpu_qpc"),
                    TraceLoggingInt64(convert(q, v[i]), "gpu_qpc"), TraceLoggingInt64(now, "now_qpc"),
                    TraceLoggingInt64((int64_t)qpf, "qpf"));
            }
            std::memset(v, 0, n * sizeof(uint64_t));
            c.slab->rb->Unmap(0, &rr);
            c.read = true;
        }
        if (open || (pending && done < r.fence + 8)) break;
        for (Slab* s : r.slabs) {
            void* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            if (SUCCEEDED(s->rb->Map(0, &none, &p)) && p) {
                std::memset(p, 0, kSlabStamps * sizeof(uint64_t));
                D3D12_RANGE all{ 0, kSlabStamps * sizeof(uint64_t) };
                s->rb->Unmap(0, &all);
            }
            g_free[s->ty].push_back(s);
        }
        g_closed.pop_front();
    }
}

} // namespace HotPath
