// ===========================================================================================
//  dx12_dump_readback.cpp -- OURS. PORT-LEAF for the ported frame-analysis engine.
//
//  THE ONE THING DX11 GAVE THEM THAT DX12 DOES NOT. Their whole dump engine funnels through a
//  single call (frame_analysis_engine.cpp: DumpBufferImmediateCtx and the texture paths):
//
//      GetDumpingContext()->Map(staging, 0, D3D11_MAP_READ, 0, &map);
//
//  On DX11 that is legal and synchronous: the immediate context has already executed the
//  CopyResource that filled `staging`, and Map blocks until the GPU is done. DX12 has no immediate
//  context and no blocking Map -- a DEFAULT-heap resource cannot be mapped at all, and a READBACK
//  one only yields meaningful bytes after a fence.
//
//  So this supplies exactly that: a private DIRECT queue + command list on which staging copies are
//  recorded, and a Map(READ) that submits, waits, and hands back the mapped pointer. Nothing above
//  it changes -- DumpBufferImmediateCtx, DumpCBs, DumpVBs, DumpIB and the rest are their code,
//  unmodified.
//
//  WHY A BLOCKING WAIT IS CORRECT HERE, and not a violation of the perf rules:
//  frame analysis is a deliberate stall. Their own engine calls DispatchInputEvents() in the middle
//  of a dump (frame_analysis_engine.cpp: dump_deferred_resources, DumpBuffer, DumpResource) purely so the game keeps responding to
//  the keyboard while it hitches for seconds. The perf rules forbid a blocking fence on the PER-FRAME
//  path; this runs only while a dump is armed.
//
//  WHY A PRIVATE QUEUE rather than the game's command list: their sequence is copy-then-map with
//  nothing in between, and the game's list is mid-recording -- we cannot submit it. Buffers make this
//  safe: D3D12 implicitly promotes a buffer from COMMON to COPY_SOURCE/COPY_DEST, so no cross-queue
//  state agreement is needed. Textures do NOT have that guarantee, which is why the texture path
//  (Dump2DResourceImmediateCtx) transitions each texture from the state the game's own barriers left it
//  in, and skips one whose state was never observed.
// ===========================================================================================
#include "dx12_dump_readback.h"
#include "dx12_wrap.h"
#include "dx12_context.h"
#include "dump_watchdog.h"
#include "../../src/log.h"
#include "../../src/d3d12_buffer.h"
#include "../../src/d3d12_barrier.h"
#include "../../src/gpu_queue.h"

#include <atomic>
#include <mutex>
#include <vector> // pre-drain cache
#include <cstdint>

namespace MigotoPort {

namespace {

struct DumpQueue {
    ID3D12CommandQueue*        queue = nullptr;
    ID3D12CommandAllocator*    alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence*               fence = nullptr;
    UINT64                     value = 0;
    bool                       open = false; // list is recording and has work
    bool                       failed = false;
    ID3D12Fence*               gfence = nullptr;
    UINT64                     gvalue = 0;
    HANDLE                     event = nullptr; // created once, reused: this waits per dumped resource
    bool                       stale = false;
};

DumpQueue                    g_dq;
std::mutex                   g_dq_mtx;
struct RetiredReadback { ID3D12Resource* res; ResourceAllocation memory; };
std::vector<RetiredReadback> g_rb_retired;

void LogQueueFailure(const char* what, HRESULT hr)
{
    static std::atomic<int> n{ 0 };
    const int               k = n.fetch_add(1);
    if (k >= 24) return;
    HRESULT removed = S_OK;
    if (const Dx12Bridge& b = GetDx12Bridge(); b.device) removed = b.device->GetDeviceRemovedReason();
    logger::write("DUMPRB: %s failed (hr=0x%08x, device removed reason 0x%08x, failure #%d) at stage '%s' detail=%llx",
        what, (unsigned)hr, (unsigned)removed, k + 1, DumpStageName(), DumpStageDetail());
}

bool Ensure()
{
    if (g_dq.list) return true;
    if (g_dq.failed) return false;

    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device) {
        static bool once = false;
        if (!once) {
            once = true;
            logger::write("DUMPRB: no DX12 device on the bridge -- readback unavailable (queue=%p cl=%p)",
                (void*)b.queue, (void*)b.cl);
        }
        return false;
    }

    // DIRECT, not COPY: a copy queue cannot do the transitions the texture path needs.
    if (FAILED(CreateGpuQueue(b.device, D3D12_COMMAND_LIST_TYPE_DIRECT, nullptr, nullptr,
            &g_dq.queue, &g_dq.alloc, &g_dq.list)) ||
        FAILED(b.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dq.fence))) ||
        FAILED(b.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dq.gfence)))) {
        logger::write("DUMPRB: could not create the private dump queue -- frame analysis readback "
                      "is unavailable");
        g_dq.failed = true;
        return false;
    }
    g_dq.open = false; // created closed; we reopen per batch
    return true;
}

bool BeginBatch()
{
    if (g_dq.stale) {
        if (g_dq.list) {
            g_dq.list->Release();
            g_dq.list = nullptr;
        }
        if (g_dq.alloc) {
            g_dq.alloc->Release();
            g_dq.alloc = nullptr;
        }
        if (g_dq.queue) {
            g_dq.queue->Release();
            g_dq.queue = nullptr;
        }
        if (g_dq.fence) {
            g_dq.fence->Release();
            g_dq.fence = nullptr;
        }
        if (g_dq.gfence) {
            g_dq.gfence->Release();
            g_dq.gfence = nullptr;
        }
        g_dq.value = g_dq.gvalue = 0;
        g_dq.open = false;
        g_dq.failed = false;
        g_dq.stale = false;
        logger::write("DUMPRB: rebuilding the readback queue after a failure -- previous captures since "
                      "that failure contain ZEROS, not data");
    }
    if (!Ensure()) return false;
    if (g_dq.open) return true;
    HRESULT hr = g_dq.alloc->Reset();
    if (FAILED(hr)) {
        LogQueueFailure("allocator Reset -- rebuilding the queue; captures taken before this rebuild are ZEROS", hr);
        g_dq.stale = true; // same recovery as the list Reset below
        return false;
    }
    hr = g_dq.list->Reset(g_dq.alloc, nullptr);
    if (FAILED(hr)) {
        LogQueueFailure("list Reset -- rebuilding the queue; captures taken before this rebuild are ZEROS", hr);
        g_dq.stale = true; // recoverable: BeginBatch rebuilds on the next call
        return false;
    }
    g_dq.open = true;
    return true;
}

} // namespace

ID3D12GraphicsCommandList* DumpReadbackList()
{
    std::lock_guard<std::mutex> l(g_dq_mtx);
    return BeginBatch() ? g_dq.list : nullptr;
}

// ---- PRE-DRAIN CACHE (see dx12_dump_readback.h) ---------------------------------------------------
namespace {

struct PreEntry {
    ID3D12Resource*      res;
    unsigned             row_pitch;
    std::vector<uint8_t> bytes;
};

constexpr unsigned kPreDrainChunkBytes = 128u * 1024u * 1024u;

ID3D12Resource*              g_pre_rb = nullptr; // one READBACK buffer, reused chunk after chunk
std::vector<PreEntry>        g_pre_list;
std::vector<ID3D12Resource*> g_pre_queue;

} // namespace

void PreDrainBegin()
{
    std::lock_guard<std::mutex> l(g_dq_mtx);
    g_pre_list.clear();
    g_pre_queue.clear();
}

void PreDrainAdd(ID3D12Resource* res)
{
    if (!res) return;
    std::lock_guard<std::mutex> l(g_dq_mtx);
    for (ID3D12Resource* r : g_pre_queue)
        if (r == res) return; // bound to several slots: copy once
    g_pre_queue.push_back(res);
}

const void* PreDrainLookup(ID3D12Resource* res, unsigned* out_bytes, unsigned* out_row_pitch)
{
    std::lock_guard<std::mutex> l(g_dq_mtx);
    if (!res) return nullptr;
    for (const PreEntry& e : g_pre_list) {
        if (e.res != res || e.bytes.empty()) continue;
        if (out_bytes) *out_bytes = (unsigned)e.bytes.size();
        if (out_row_pitch) *out_row_pitch = e.row_pitch;
        return e.bytes.data();
    }
    return nullptr;
}

void PreDrainFlush()
{
    std::lock_guard<std::mutex> l(g_dq_mtx);
    if (g_pre_queue.empty() || !Ensure()) return;

    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device) return;

    if (!g_pre_rb) {
        if (FAILED(CreateD3D12Buffer(b.device, D3D12_HEAP_TYPE_READBACK, kPreDrainChunkBytes,
                D3D12_RESOURCE_STATE_COPY_DEST, &g_pre_rb)))
            return;
    }

    struct Pending {
        ID3D12Resource* res;
        unsigned        offset;
        unsigned        bytes;
        unsigned        row_pitch;
    };
    std::vector<Pending> chunk;
    size_t               i = 0;
    while (i < g_pre_queue.size()) {
        if (!BeginBatch()) return;
        chunk.clear();
        unsigned cursor = 0;
        bool     any = false;

        for (; i < g_pre_queue.size(); ++i) {
            ID3D12Resource* res = g_pre_queue[i];
            const uint32_t  live = ObservedResourceState(res); // COMMON (0) is a real answer: a mod texture
            if (live == kResStateUnknown) {                    // unknown -> skip, never guess
                static bool once = false;
                if (!once) {
                    once = true;
                    logger::write("DUMPRB: pre-drain skipping textures with no tracked barrier state "
                                  "-- they will fall back to the per-resource path");
                }
                continue;
            }
            const D3D12_RESOURCE_DESC rd = res->GetDesc();
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue; // buffers already cheap

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT                               rows = 0;
            UINT64                             rsz = 0;
            UINT64                             tot = 0;
            b.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rsz, &tot);
            const unsigned base = (cursor + 511u) & ~511u;
            if ((UINT64)base + tot > kPreDrainChunkBytes) {
                if (any) break;
                continue;
            }
            fp.Offset = base;

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_pre_rb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = res;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            CopyAsSource(g_dq.list, res, (D3D12_RESOURCE_STATES)live, [&] { g_dq.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); }, /*subresource*/ 0);

            chunk.push_back({ res, base, (unsigned)tot, fp.Footprint.RowPitch });
            cursor = base + (unsigned)tot;
            any = true;
        }

        if (!any) {
            g_dq.list->Close();
            g_dq.open = false;
            break;
        }

        DumpStage("PreDrain: submit chunk", cursor);
        // ONE submit + ONE fence for the whole chunk -- the entire point.
        if (const HRESULT chr = g_dq.list->Close(); FAILED(chr)) {
            LogQueueFailure("pre-drain list Close", chr);
            // The chunk's resources, so the invalid copy can be named without a debug layer.
            static int listed = 0;
            if (listed++ < 2)
                for (const Pending& c : chunk) {
                    const D3D12_RESOURCE_DESC cd = c.res->GetDesc();
                    logger::write("DUMPRB:   chunk res=%p %llux%u mips=%u array=%u fmt=%u flags=0x%x samples=%u state=0x%x bytes=%u",
                        (void*)c.res, (unsigned long long)cd.Width, cd.Height, cd.MipLevels, cd.DepthOrArraySize,
                        (unsigned)cd.Format, (unsigned)cd.Flags, cd.SampleDesc.Count, ObservedResourceState(c.res), c.bytes);
                }
            g_dq.open = false;
            g_dq.stale = true;
            return;
        }
        if (const ID3D12CommandQueue* gq = GetDx12Bridge().queue) {
            ID3D12CommandQueue* game_queue = const_cast<ID3D12CommandQueue*>(gq);
            const UINT64        gv = ++g_dq.gvalue;
            if (SUCCEEDED(game_queue->Signal(g_dq.gfence, gv))) g_dq.queue->Wait(g_dq.gfence, gv);
        }
        ID3D12CommandList* lists[] = { g_dq.list };
        g_dq.queue->ExecuteCommandLists(1, lists);
        const UINT64 v = ++g_dq.value;
        if (SUCCEEDED(g_dq.queue->Signal(g_dq.fence, v)) && g_dq.fence->GetCompletedValue() < v) {
            if (!g_dq.event) g_dq.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_dq.event) {
                DumpStage("PreDrain: fence wait", v);
                const DWORD wr = WaitForFence(g_dq.fence, v, g_dq.event, 10000);
                if (wr != WAIT_OBJECT_0) {
                    logger::write("DUMPRB: fence wait returned %lu (NOT signalled) after a %u-byte chunk "
                                  "-- the GPU still owns this allocator. Abandoning the batch instead of "
                                  "resetting under it; this chunk's bytes are NOT valid.",
                        (unsigned long)wr, cursor);
                    g_dq.open = false;
                    g_dq.stale = true; // rebuild before the next batch
                    return;            // do NOT Map: that would publish whatever the buffer held
                }
            }
        }
        g_dq.open = false;

        DumpStage("PreDrain: map + copy out", cursor);
        // Take the bytes out before the buffer is reused by the next chunk.
        void*       p = nullptr;
        D3D12_RANGE rr{ 0, cursor };
        if (SUCCEEDED(g_pre_rb->Map(0, &rr, &p)) && p) {
            static bool once = false;
            if (!once) {
                once = true;
                size_t zero_entries = 0;
                for (const Pending& c : chunk) {
                    const uint8_t* src = (const uint8_t*)p + c.offset;
                    bool           nz = false;
                    for (unsigned k = 0; k < c.bytes && !nz; ++k)
                        if (src[k]) nz = true;
                    if (!nz) ++zero_entries;
                }
                logger::write("DUMPRB: pre-drain chunk copied %zu textures, %zu of them ALL ZERO "
                              "(queued=%zu cursor=%u)",
                    chunk.size(), zero_entries, g_pre_queue.size(), cursor);
            }
            for (const Pending& c : chunk) {
                PreEntry e;
                e.res = c.res;
                e.row_pitch = c.row_pitch;
                e.bytes.assign((const uint8_t*)p + c.offset, (const uint8_t*)p + c.offset + c.bytes);
                g_pre_list.push_back(std::move(e));
            }
            D3D12_RANGE none{ 0, 0 };
            g_pre_rb->Unmap(0, &none);
        }
    }
    g_pre_queue.clear();
}

void RetireReadback(ID3D12Resource* res, ResourceAllocation memory)
{
    std::lock_guard<std::mutex> l(g_dq_mtx);
    if (g_dq.open)
        g_rb_retired.push_back({ res, memory });
    else if (g_dq.stale)
        RetireDx12Resource(res, memory);
    else {
        res->Release();
        ResourceMemory().Released(memory);
    }
}

static bool FlushLocked();

bool DumpReadbackFlush()
{
    DumpStage("Flush: wait g_dq_mtx");
    std::lock_guard<std::mutex> l(g_dq_mtx);
    const bool                  ok = FlushLocked();
    for (const auto& r : g_rb_retired) {
        if (ok) {
            r.res->Release();
            ResourceMemory().Released(r.memory);
        } else
            RetireDx12Resource(r.res, r.memory);
    }
    g_rb_retired.clear();
    return ok;
}

static bool FlushLocked()
{
    if (!g_dq.list || !g_dq.open) return true; // nothing recorded; the data is already there

    DumpStage("Flush: close");
    if (const HRESULT chr = g_dq.list->Close(); FAILED(chr)) {
        // A list that would not close is poisoned: its next Reset fails too, so rebuild it now.
        LogQueueFailure("per-resource list Close (the recorded copy was invalid; this resource is NOT mapped)", chr);
        g_dq.open = false;
        g_dq.stale = true;
        return false;
    }

    DumpStage("Flush: order after the game queue");
    if (const ID3D12CommandQueue* gq = GetDx12Bridge().queue) {
        ID3D12CommandQueue* game_queue = const_cast<ID3D12CommandQueue*>(gq);
        const UINT64        gv = ++g_dq.gvalue;
        if (SUCCEEDED(game_queue->Signal(g_dq.gfence, gv)))
            g_dq.queue->Wait(g_dq.gfence, gv);
    }

    DumpStage("Flush: execute");
    ID3D12CommandList* lists[] = { g_dq.list };
    g_dq.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++g_dq.value;
    if (FAILED(g_dq.queue->Signal(g_dq.fence, v))) {
        g_dq.open = false;
        return false;
    }

    if (g_dq.fence->GetCompletedValue() < v) {
        if (!g_dq.event) g_dq.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DumpStage("Flush: fence wait", v);
        const DWORD wr = WaitForFence(g_dq.fence, v, g_dq.event, 10000); // bounded: a hung GPU must not hang the game too
        if (wr != WAIT_OBJECT_0) {
            logger::write("DUMPRB: per-resource fence wait returned %lu (NOT signalled) -- the GPU still owns "
                          "this allocator; this resource is NOT mapped, the queue is rebuilt",
                (unsigned long)wr);
            g_dq.open = false;
            g_dq.stale = true;
            return false;
        }
    }
    g_dq.open = false;
    DumpStage("Flush: done");
    return true;
}

} // namespace MigotoPort
