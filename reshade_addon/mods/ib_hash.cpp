// ib_hash.cpp -- see ib_hash.h. GPU crc32c of the char VB prefix (in-place SRV read, 1024 slice CRCs read
// back and combined here) + CPU desc chain = 3dmigoto's ini `hash=`. Bounded: one dispatch per distinct
// (res,offset,byteLen), cached.
#include "ib_hash.h"
#include "wwmi_runtime.h" // WwmiReadFile / WwmiMakeBuffer
#include "../migoto/port_api.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/addon.h"
#include "mods_toggles.h"
#include "../instrumentation/gpu_timer.h"
#include "../hooks/resource_tracker.h" // ResourceTracker::StateOf: the state the game's barriers left a buffer in
#include "../migoto/dx12_bridge.h" // kResStateUnknown
#include "../core/log.h"
#include "../core/d3d12_barrier.h"
#include <map>
#include <wrl.h>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <atomic>

ModsToggles g_mods_toggles;

using Microsoft::WRL::ComPtr;

namespace {
std::shared_mutex g_mtx; // shared for the per-draw cache read, exclusive for every change
bool g_ready = false;
bool g_failed = false;
std::atomic<bool> g_ready_fast{ false }; // g_ready, readable without g_mtx
ComPtr<ID3D12RootSignature> g_rs;
ComPtr<ID3D12PipelineState> g_pso;
ComPtr<ID3D12Resource> g_out; // DEFAULT UAV
ComPtr<ID3D12Resource> g_rb; // READBACK
ID3D12Fence* g_fence = nullptr; // the present pass's fence and the value that completes the recorded list
UINT64 g_fv = 0;

struct Job {
    ID3D12Resource* ib;
    ID3D12Resource* vb;
    uint32_t ib_off;
    uint32_t ib_bytes;
    uint32_t idx_size;
    uint32_t vb_off;
    uint32_t vb_stride;
    uint64_t vb_va;
};
std::vector<Job> g_pending;
std::vector<Job> g_small_pending; // LookupSmall's jobs: recorded when g_pending is empty
std::unordered_set<uint64_t> g_small_queued; // their vb_va
struct Cached {
    uint32_t hash;
    uint32_t ib_bytes; // the index extent the hash was taken over
};
std::map<uint64_t, Cached> g_cache;
std::atomic<uint32_t> g_cache_gen{ 1 };
struct LookupMemo {
    uint64_t vb_va;
    uint32_t hash;
    uint32_t ib_bytes;
    uint32_t gen;
};
thread_local LookupMemo t_memo[1024] = {};

void StoreLocked(uint64_t vb_va, const Cached& c)
{
    if (!g_cache.insert_or_assign(vb_va, c).second) g_cache_gen.fetch_add(1, std::memory_order_acq_rel);
}
// one hash in flight at a time (NON-BLOCKING: dispatch, read the result a later present -- never stall).
bool g_inflight = false;
UINT64 g_inflight_fv = 0;
Job g_inflight_job = {};
bool g_inflight_stale = false; // its region was rewritten while it computed: do not cache
bool g_recorded = false; // Record wrote a dispatch this frame; Submitted names its fence
LARGE_INTEGER g_inflight_t0 = {};
uint64_t g_inflight_ext = 0; // vb view extent (vbW - vb_off) of the in-flight job
std::atomic<uint32_t> g_ibhash_slow_logged{ 0 };
std::atomic<uint32_t> g_ibhash_skip_logged{ 0 };
LARGE_INTEGER g_rate_t0 = {};
uint32_t g_rate_jobs = 0;
uint32_t g_rate_slow = 0;
uint64_t g_rate_bytes = 0;
double g_rate_max_ms = 0.0;

double QpcMs(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
{
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
}

// THE SHADER'S OUTPUT: a header and one (crc, bytes) per thread. See IbVbHash.hlsl.
static constexpr uint32_t kThreads = 1024;
static constexpr uint32_t kOutBytes = 8 + kThreads * 8;

uint32_t gf2_times(const uint32_t* mat, uint32_t vec)
{
    uint32_t sum = 0;
    for (; vec; vec >>= 1, ++mat)
        if (vec & 1u) sum ^= *mat;
    return sum;
}

void gf2_square(uint32_t* square, const uint32_t* mat)
{
    for (int n = 0; n < 32; ++n) square[n] = gf2_times(mat, mat[n]);
}

struct CombineOp {
    uint64_t len = 0;
    uint32_t m[32] = {};

    uint32_t operator()(uint32_t crc1, uint32_t crc2, uint64_t len2)
    {
        if (len2 == 0) return crc1;
        if (len2 != len) {
            len = len2;
            build(len2);
        }
        return gf2_times(m, crc1) ^ crc2;
    }

    void build(uint64_t bytes)
    {
        uint32_t base[32];
        uint32_t sq[32];
        uint32_t acc[32];
        base[0] = 0x82F63B78u; // one zero bit: the reflected polynomial
        for (int n = 1; n < 32; ++n) base[n] = 1u << (n - 1);
        for (int k = 0; k < 3; ++k) { // 8 bits
            gf2_square(sq, base);
            for (int n = 0; n < 32; ++n) base[n] = sq[n];
        }
        for (int n = 0; n < 32; ++n) m[n] = 1u << n; // identity
        for (;;) {
            if (bytes & 1u) {
                for (int n = 0; n < 32; ++n) acc[n] = gf2_times(base, m[n]);
                for (int n = 0; n < 32; ++n) m[n] = acc[n];
            }
            bytes >>= 1;
            if (!bytes) break;
            gf2_square(sq, base);
            for (int n = 0; n < 32; ++n) base[n] = sq[n];
        }
    }
};

uint32_t chain_desc(uint32_t data_crc, uint32_t byteWidth)
{
    uint32_t desc[6] = { byteWidth, 0u, 0x9u, 0u, 0u, 0u };
    return PortApi::Crc32c(data_crc, desc, sizeof(desc));
}
}

namespace IbHash {

bool Init(ID3D12Device* dev)
{
    if (g_ready_fast.load(std::memory_order_acquire)) return true;
    std::unique_lock l(g_mtx);
    if (g_ready) return true;
    if (g_failed || !dev) return false;
    D3D12_ROOT_PARAMETER rp[4] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor.ShaderRegister = 0; // t0 = IB
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[2].Descriptor.ShaderRegister = 1; // t1 = VB
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[3].Descriptor.ShaderRegister = 0; // u0 = out (crc,vbBytes)
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 4;
    rsd.pParameters = rp;
    ComPtr<ID3DBlob> bl;
    ComPtr<ID3DBlob> er;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &bl, &er))) {
        logger::write("IbHash: rootsig fail");
        g_failed = true;
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, bl->GetBufferPointer(), bl->GetBufferSize(), IID_PPV_ARGS(&g_rs)))) {
        g_failed = true;
        return false;
    }
    auto cso = WwmiReadFile((g_addon.cso_dir + L"\\IbVbHash.cso").c_str());
    if (cso.empty()) {
        logger::write("IbHash: IbVbHash.cso missing");
        g_failed = true;
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_rs.Get();
    pd.CS.pShaderBytecode = cso.data();
    pd.CS.BytecodeLength = cso.size();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) {
        logger::write("IbHash: PSO fail");
        g_failed = true;
        return false;
    }
    g_out = WwmiMakeBuffer(dev, 16384, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_rb =
        WwmiMakeBuffer(dev, 16384, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    static_assert(kOutBytes <= 16384, "IbHash: the slice table must fit the readback");
    if (!g_out || !g_rb) {
        g_failed = true;
        return false;
    }
    g_ready = true;
    g_ready_fast.store(true, std::memory_order_release);
    logger::write("IbHash: initialized (GPU crc32c, in the present pass)");
    return true;
}

uint32_t Lookup(ID3D12Resource* ib_res, uint32_t ib_off, uint32_t ib_bytes, uint32_t idx_size, ID3D12Resource* vb_res,
    uint32_t vb_off, uint32_t vb_stride, bool grow)
{
    if (!ib_res || !vb_res || !ib_bytes || !idx_size || !vb_stride) return 0;
    if (!Init(g_addon.d3d12_device)) return 0;
    uint64_t vb_va = vb_res->GetGPUVirtualAddress() + vb_off; // UNIQUE per char (pooled VBs)
    LookupMemo& m = t_memo[((vb_va >> 8) ^ (vb_va >> 20)) & 1023];
    const uint32_t gen = g_cache_gen.load(std::memory_order_acquire);
    if (m.vb_va == vb_va && m.gen == gen && !(grow && m.hash && ib_bytes > m.ib_bytes)) return m.hash;
    uint32_t meanwhile = 0;
    {
        std::shared_lock l(g_mtx); // the hot path: a cached hash, read in parallel
        auto it = g_cache.find(vb_va);
        const bool further = grow && it != g_cache.end() && it->second.hash && ib_bytes > it->second.ib_bytes;
        if (it != g_cache.end() && !further) {
            m = LookupMemo{ vb_va, it->second.hash, it->second.ib_bytes, gen };
            return it->second.hash;
        }
        meanwhile = further ? it->second.hash : 0;
        if (g_inflight && g_inflight_job.vb_va == vb_va) return meanwhile; // being computed
    }
    std::unique_lock l(g_mtx);
    if (g_inflight && g_inflight_job.vb_va == vb_va) return meanwhile;
    for (auto& j : g_pending) {
        if (j.vb_va == vb_va) { // dedup pending; a queued job may still widen
            if (grow && ib_bytes > j.ib_bytes) j.ib_bytes = ib_bytes;
            return meanwhile;
        }
    }
    if (g_pending.size() < 32) {
        ib_res->AddRef();
        vb_res->AddRef();
        g_pending.push_back({ ib_res, vb_res, ib_off, ib_bytes, idx_size, vb_off, vb_stride, vb_va });
    }
    return meanwhile;
}

uint32_t LookupSmall(ID3D12Resource* ib_res, uint32_t ib_off, uint32_t ib_bytes, uint32_t idx_size,
    ID3D12Resource* vb_res, uint32_t vb_off, uint32_t vb_stride)
{
    if (!ib_res || !vb_res || !ib_bytes || !idx_size || !vb_stride) return 0;
    if (!Init(g_addon.d3d12_device)) return 0;
    const uint64_t vb_va = vb_res->GetGPUVirtualAddress() + vb_off;
    LookupMemo& m = t_memo[((vb_va >> 8) ^ (vb_va >> 20)) & 1023];
    const uint32_t gen = g_cache_gen.load(std::memory_order_acquire);
    if (m.vb_va == vb_va && m.gen == gen) return m.hash;
    {
        std::shared_lock l(g_mtx);
        auto it = g_cache.find(vb_va);
        if (it != g_cache.end()) {
            m = LookupMemo{ vb_va, it->second.hash, it->second.ib_bytes, gen };
            return it->second.hash;
        }
        if ((g_inflight && g_inflight_job.vb_va == vb_va) || g_small_queued.count(vb_va)) return 0;
    }
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
    const bool cpu_written = SUCCEEDED(vb_res->GetHeapProperties(&hp, &hf)) && hp.Type != D3D12_HEAP_TYPE_DEFAULT;
    std::unique_lock l(g_mtx);
    if (g_cache.count(vb_va) || (g_inflight && g_inflight_job.vb_va == vb_va) || g_small_queued.count(vb_va)) return 0;
    if (cpu_written) {
        StoreLocked(vb_va, Cached{ 0, ib_bytes });
        return 0;
    }
    for (const Job& j : g_pending)
        if (j.vb_va == vb_va) return 0;
    ib_res->AddRef();
    vb_res->AddRef();
    g_small_pending.push_back({ ib_res, vb_res, ib_off, ib_bytes, idx_size, vb_off, vb_stride, vb_va });
    g_small_queued.insert(vb_va);
    return 0;
}

void ForgetRange(uint64_t va, uint64_t bytes)
{
    if (!va || !bytes) return;
    {
        std::shared_lock l(g_mtx);
        bool hit = g_cache.lower_bound(va) != g_cache.lower_bound(va + bytes) ||
            (g_inflight && g_inflight_job.vb_va >= va && g_inflight_job.vb_va < va + bytes);
        for (size_t i = 0; i < g_pending.size() && !hit; ++i)
            hit = g_pending[i].vb_va >= va && g_pending[i].vb_va < va + bytes;
        for (size_t i = 0; i < g_small_pending.size() && !hit; ++i)
            hit = g_small_pending[i].vb_va >= va && g_small_pending[i].vb_va < va + bytes;
        if (!hit) return;
    }
    std::vector<Job> dropped;
    {
        std::unique_lock l(g_mtx);
        g_cache.erase(g_cache.lower_bound(va), g_cache.lower_bound(va + bytes));
        g_cache_gen.fetch_add(1, std::memory_order_acq_rel);
        if (g_inflight && g_inflight_job.vb_va >= va && g_inflight_job.vb_va < va + bytes) g_inflight_stale = true;
        for (size_t i = 0; i < g_pending.size();) {
            if (g_pending[i].vb_va >= va && g_pending[i].vb_va < va + bytes) {
                dropped.push_back(g_pending[i]);
                g_pending.erase(g_pending.begin() + i);
            } else {
                ++i;
            }
        }
        for (size_t i = 0; i < g_small_pending.size();) {
            if (g_small_pending[i].vb_va >= va && g_small_pending[i].vb_va < va + bytes) {
                dropped.push_back(g_small_pending[i]);
                g_small_queued.erase(g_small_pending[i].vb_va);
                g_small_pending.erase(g_small_pending.begin() + i);
            } else {
                ++i;
            }
        }
    }
    for (const Job& j : dropped) {
        j.ib->Release();
        j.vb->Release();
    }
}

void DrainQueue()
{
    if (g_inflight) {
        if (g_recorded || !g_fence || g_fence->GetCompletedValue() < g_inflight_fv)
            return; // still computing -- check next present
        void* p = nullptr;
        D3D12_RANGE rr = { 0, kOutBytes };
        uint32_t data_crc = 0;
        uint32_t vbBytes = 0;
        if (SUCCEEDED(g_rb->Map(0, &rr, &p)) && p) {
            const uint32_t* o = reinterpret_cast<const uint32_t*>(p);
            vbBytes = o[0]; // clamped to the view by the shader
            data_crc = o[2];
            CombineOp combine;
            for (uint32_t k = 1; k < kThreads; ++k) data_crc = combine(data_crc, o[2 + k * 2], o[3 + k * 2]);
            D3D12_RANGE w = { 0, 0 };
            g_rb->Unmap(0, &w);
        }
        uint32_t full = chain_desc(data_crc, vbBytes);
        if (!vbBytes) // the shader cannot write 0 here: the dispatch did not run
            logger::write(
                "IBHASH-ZERO: a collected job holds vbBytes=0 -- its dispatch never ran | ib_off=%u ibBytes=%u idx=%u stride=%u vbExtent=%llu vb_va=%llx",
                g_inflight_job.ib_off, g_inflight_job.ib_bytes, g_inflight_job.idx_size, g_inflight_job.vb_stride,
                (unsigned long long)g_inflight_ext, (unsigned long long)g_inflight_job.vb_va);
        if (g_debug) {
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            const double ms = QpcMs(g_inflight_t0, t1);
            const Job& ij = g_inflight_job;
            const bool slow = ms >= 100.0 || vbBytes >= (1u << 20);
            ++g_rate_jobs;
            g_rate_bytes += vbBytes;
            if (ms > g_rate_max_ms) g_rate_max_ms = ms;
            if (slow) ++g_rate_slow;
            if (slow && g_ibhash_slow_logged.fetch_add(1, std::memory_order_relaxed) < 2000)
                logger::write(
                    "IBHASH-SLOW %.1f ms dispatch-to-seen | vbBytes=%u (%u vertices x stride %u) vbExtent=%llu B"
                    " | ib_off=%u ibBytes=%u idx=%u (%u indices) vb_va=%llx",
                    ms, vbBytes, ij.vb_stride ? vbBytes / ij.vb_stride : 0u, ij.vb_stride,
                    (unsigned long long)g_inflight_ext, ij.ib_off, ij.ib_bytes, ij.idx_size, ij.ib_bytes / ij.idx_size,
                    (unsigned long long)ij.vb_va);
            if (!g_rate_t0.QuadPart) g_rate_t0 = t1;
            if (QpcMs(g_rate_t0, t1) >= 10000.0) {
                logger::write(
                    "IBHASH-RATE last %.0f s: %u hashes, %llu VB bytes, slowest %.1f ms, %u slow (>=100 ms or >=1 MB)",
                    QpcMs(g_rate_t0, t1) / 1000.0, g_rate_jobs, (unsigned long long)g_rate_bytes, g_rate_max_ms,
                    g_rate_slow);
                g_rate_t0 = t1;
                g_rate_jobs = 0;
                g_rate_slow = 0;
                g_rate_bytes = 0;
                g_rate_max_ms = 0.0;
            }
        }
        logger::trace("IBHASH vb_va=%llx ibBytes=%u idx=%u stride=%u vbBytes=%u dataCrc=%08x FULL(vbHash)=%08x",
            (unsigned long long)g_inflight_job.vb_va, g_inflight_job.ib_bytes, g_inflight_job.idx_size,
            g_inflight_job.vb_stride, vbBytes, data_crc, full);
        Job done;
        {
            std::unique_lock l(g_mtx);
            if (vbBytes && !g_inflight_stale) {
                StoreLocked(g_inflight_job.vb_va,
                    Cached{ full, g_inflight_job.ib_bytes }); // 0 bytes: not run, the next draw requeues
            }
            done = g_inflight_job;
            g_inflight = false;
            g_inflight_stale = false;
        }
        done.ib->Release();
        done.vb->Release();
    }
}

static D3D12_RESOURCE_STATES SrvStateOf(ID3D12Resource* res)
{
    const uint32_t s = g_resources.StateOf(res);
    if (s == MigotoPort::kResStateUnknown || s == (uint32_t)D3D12_RESOURCE_STATE_COMMON || s == kTrackedCommon)
        return D3D12_RESOURCE_STATE_COMMON;
    return (D3D12_RESOURCE_STATES)s;
}

bool Record(ID3D12GraphicsCommandList* cl)
{
    if (!cl || g_inflight) return false; // one hash in flight at a time
    Job j;
    {
        std::unique_lock l(g_mtx);
        if (!g_ready) return false;
        if (!g_pending.empty()) {
            j = g_pending.front();
            g_pending.erase(g_pending.begin());
        } else if (!g_small_pending.empty()) {
            j = g_small_pending.front();
            g_small_pending.erase(g_small_pending.begin());
        } else {
            return false;
        }
    }
    uint64_t ibW = j.ib->GetDesc().Width;
    uint64_t vbW = j.vb->GetDesc().Width;
    if ((uint64_t)j.ib_off + j.ib_bytes > ibW || j.vb_off >= vbW) { // bad range -> cache 0, skip
        {
            std::unique_lock l(g_mtx);
            StoreLocked(j.vb_va, Cached{ 0, j.ib_bytes });
            g_small_queued.erase(j.vb_va);
        }
        j.ib->Release();
        j.vb->Release();
        return false;
    }
    if (g_mods_toggles.ibhash_gpu_skip) { // X35: same outcome as a bad range -- cached 0, no dispatch
        if (g_debug && g_ibhash_skip_logged.fetch_add(1, std::memory_order_relaxed) < 50)
            logger::write(
                "IBHASH-SKIP vb_va=%llx ibBytes=%u idx=%u stride=%u vbExtent=%llu B -- X35 on, cached 0, not dispatched",
                (unsigned long long)j.vb_va, j.ib_bytes, j.idx_size, j.vb_stride, (unsigned long long)(vbW - j.vb_off));
        {
            std::unique_lock l(g_mtx);
            StoreLocked(j.vb_va, Cached{ 0, j.ib_bytes });
            g_small_queued.erase(j.vb_va);
        }
        j.ib->Release();
        j.vb->Release();
        return false;
    }
    const D3D12_RESOURCE_STATES ib_st = SrvStateOf(j.ib);
    const D3D12_RESOURCE_STATES vb_st = SrvStateOf(j.vb);
    const bool ib_tr =
        ib_st != D3D12_RESOURCE_STATE_COMMON && !(ib_st & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool vb_tr = vb_st != D3D12_RESOURCE_STATE_COMMON &&
        !(vb_st & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && j.vb != j.ib;
    if (ib_tr) TransitionBarrier(cl, j.ib, ib_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (vb_tr) TransitionBarrier(cl, j.vb, vb_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl->SetPipelineState(g_pso.Get());
    cl->SetComputeRootSignature(g_rs.Get());
    const uint64_t vb_ext = vbW - j.vb_off; // the view's extent: the shader clamps to it
    uint32_t consts[4] = { j.ib_bytes, j.idx_size, j.vb_stride,
        vb_ext > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)vb_ext };
    cl->SetComputeRoot32BitConstants(0, 4, consts, 0);
    cl->SetComputeRootShaderResourceView(1, j.ib->GetGPUVirtualAddress() + j.ib_off); // t0 IB
    cl->SetComputeRootShaderResourceView(2, j.vb->GetGPUVirtualAddress() + j.vb_off); // t1 VB
    cl->SetComputeRootUnorderedAccessView(3, g_out->GetGPUVirtualAddress());
    const int gt = g_gpu_timer.Begin(cl, GT_IbHash);
    void* const hp = HotPath::GpuBracketBegin(cl, GT_IbHash);
    cl->Dispatch(1, 1, 1);
    HotPath::GpuBracketEnd(cl, hp);
    g_gpu_timer.End(cl, gt);
    if (ib_tr) TransitionBarrier(cl, j.ib, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ib_st);
    if (vb_tr) TransitionBarrier(cl, j.vb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, vb_st);
    CopyAsSource(cl, g_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        [&] { cl->CopyBufferRegion(g_rb.Get(), 0, g_out.Get(), 0, kOutBytes); });
    QueryPerformanceCounter(&g_inflight_t0);
    g_inflight_ext = vbW - j.vb_off;
    {
        std::unique_lock l(g_mtx); // the fence comes with Submitted
        g_inflight = true;
        g_recorded = true;
        g_inflight_job = j;
        g_small_queued.erase(j.vb_va);
    }
    return true;
}

void Submitted(ID3D12Fence* fence, uint64_t value)
{
    std::unique_lock l(g_mtx);
    if (!g_recorded) return;
    g_recorded = false;
    if (!g_inflight) return;
    g_fence = fence; // read the result once this value passes
    g_inflight_fv = value;
}

void LogInflight(const char* why)
{
    if (!g_inflight) {
        logger::write("IBHASH-INFLIGHT (%s): no hash dispatch was in flight", why);
        return;
    }
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    const Job& ij = g_inflight_job;
    logger::write("IBHASH-INFLIGHT (%s): a hash was dispatched %.1f ms earlier and never collected | ib_off=%u "
                  "ibBytes=%u idx=%u (%u indices) stride=%u vbExtent=%llu B vb_va=%llx",
        why, QpcMs(g_inflight_t0, t1), ij.ib_off, ij.ib_bytes, ij.idx_size,
        ij.idx_size ? ij.ib_bytes / ij.idx_size : 0u, ij.vb_stride, (unsigned long long)g_inflight_ext,
        (unsigned long long)ij.vb_va);
}

} // namespace IbHash
