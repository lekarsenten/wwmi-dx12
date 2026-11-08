// SKCAP: the compute-chain capture. See the banner below.
#include "dx12_skcap.h"
#include "dx12_pipeline.h"
#include "dx12_pipeline_internal.h"
#include "dx12_wrap.h"
#include "dx12_context.h"
#include "../core/log.h"
#include "../core/fnv.h"
#include "../core/d3d12_buffer.h"
#include "../core/d3d12_barrier.h"
#include "port_api.h" // BridgeStackDepth, GameComputeUavBufferHash, ForEachTextureOverrideConfig, ...

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MigotoPort {

// =================================================================================================
//  SKCAP -- compute-chain capture that can actually SEE A DISPATCH'S OUTPUT.
//
//  WHY THE FRAME-ANALYSIS DUMP CANNOT. dx12_dump_readback.cpp records its staging copies on a PRIVATE
//  queue and orders them with
//        game_queue->Signal(gfence, v);  our_queue->Wait(gfence, v);
//  which means "wait for everything the game has SUBMITTED". Our dispatches are recorded into the
//  game's CURRENTLY-OPEN command list, which has not been submitted yet -- so the copy reads the
//  buffer as it stood BEFORE this frame's dispatches. Every UAV in such a capture is therefore
//  pre-dispatch, which says nothing about whether the shader wrote it.
//
//  The private queue was chosen because their engine's flow is copy-then-Map with nothing in between
//  and the game's list cannot be submitted mid-recording (see that file's header). That reasoning is
//  right for their SYNCHRONOUS flow and wrong for outputs. So this records CopyBufferRegion INTO THE
//  GAME'S OWN LIST, right after the Dispatch, and reads it back some frames later once that list has
//  actually executed. GPU ordering inside one list is guaranteed, so the bytes are genuinely
//  post-dispatch.
//
//  Scope is deliberately the compute chain only -- a full frame capture costs 129 GB (measured), 78%
//  of it pixel-shader textures that have nothing to do with shapekeys.
// =================================================================================================
namespace {

struct SkCapSlot {
    char owner = 'M';
    char prov[72] = {};
    std::string name; // "u0", "cs-t33", "cs-cb0" ...
    bool post = false; // false = snapshot taken BEFORE the Dispatch
    ID3D12Resource* staging = nullptr; // ours, READBACK heap
    UINT64 bytes = 0; // how much we copied
    UINT64 src_size = 0; // how big the source actually is
    UINT64 src_off = 0; // window offset into the game's allocation
    const void* src_res = nullptr; // identity, for correlating pre/post and across passes
    unsigned long long src_va = 0;
    unsigned seq = 0; // which captured dispatch
    const void* cs = nullptr; // that dispatch's CS wrapper
    UINT gx = 0; // its thread-group dims
    UINT gy = 0;
    UINT gz = 0;
    char view[208] = {};
    unsigned frame = 0; // capture-relative frame this snapshot was taken on
    bool done = false;
    std::string digest;
    std::vector<uint8_t> keep;
};

struct SkCapPass {
    uint32_t uav_bound_mask = 0; // which compute uN actually reached a root parameter
    unsigned long long t_begin_us = 0;
    unsigned long long t_end_us = 0;
    unsigned long long caller_cs = 0;
    unsigned caller_tgy = 0;
    int caller_mod = -1;
    wchar_t caller_ns[64] = { 0 };
    unsigned seq = 0;
    unsigned frame = 0; // capture-relative frame this dispatch issued on
    const void* cs = nullptr;
    UINT gx = 0;
    UINT gy = 0;
    UINT gz = 0;
    std::vector<uint8_t> code; // CS bytecode, written out as .cso
    std::vector<ShaderReg> regs; // reflected declarations: kind b/t/u/s, reg, space
};

struct SkCapSkip {
    std::string name;
    bool post = false;
    unsigned seq = 0;
    std::string why;
};

std::mutex g_skcap_mtx;
std::vector<SkCapSlot> g_skcap;
std::vector<SkCapSkip> g_skcap_skip;
std::vector<SkCapPass> g_skcap_pass;
static std::mutex g_skcap_caller_mtx;
static unsigned long long g_skcap_caller_cs = 0;
static unsigned g_skcap_caller_tgy = 0;
static int g_skcap_caller_mod = -1;
static wchar_t g_skcap_caller_ns[64] = { 0 };
static LARGE_INTEGER g_skcap_t0 = {};
static LARGE_INTEGER g_skcap_freq = {};
static unsigned long long SkCapNowUs()
{
    if (!g_skcap_freq.QuadPart) QueryPerformanceFrequency(&g_skcap_freq);
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    if (!g_skcap_t0.QuadPart) g_skcap_t0 = n;
    return (unsigned long long)((n.QuadPart - g_skcap_t0.QuadPart) * 1000000LL / g_skcap_freq.QuadPart);
}
std::atomic<int> g_skcap_arm{ 0 }; // dispatches still to capture
std::atomic<int> g_skcap_wait{ 0 }; // frames to wait before the readback is valid
std::atomic<int> g_skcap_frames_left{ 0 };
std::atomic<unsigned> g_skcap_frame{ 0 }; // capture-relative frame index stamped onto every record
std::atomic<int> g_skcap_frames_armed{ 0 };
std::atomic<int> g_skcap_mode{ 0 };
unsigned g_skcap_seq = 0;
UINT64 g_skcap_budget = 0; // bytes still allowed to stage

constexpr UINT64 kSkCapPerResource = 32ull << 20; // never copy more than this from one resource
constexpr UINT64 kSkCapTotalBudget = 1536ull << 20; // and never stage more than this in one capture
constexpr UINT64 kSkCapFullWrite = 1ull << 20;

constexpr UINT64 kSkCapGameShareDiv = 4;
UINT64 g_skcap_game_budget = 0; // FRAME mode: bytes the G rows may still stage this frame
struct SkCapDSeen {
    unsigned frame;
    const void* cl;
    const void* res;
    UINT64 off;
};
std::vector<SkCapDSeen> g_skcap_d_seen; // FRAME mode: D rows already staged this frame
unsigned g_skcap_d_dedup = 0; // D rows not staged because rule 3 already had them

ID3D12Resource* SkCapMakeReadback(ID3D12Device* dev, UINT64 bytes)
{
    ID3D12Resource* out = nullptr;
    if (FAILED(CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, &out)))
        return nullptr;
    return out;
}

void SkCapSnap(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const char* name, void* iface, bool is_view, bool post,
    const void* cs, unsigned seq, UINT gx, UINT gy, UINT gz)
{
    auto skip = [&](const char* why) {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        g_skcap_skip.push_back(SkCapSkip{ name, post, seq, why });
    };
    if (!iface || !dev || !cl) {
        skip("no iface/device/list");
        return;
    }
    WrapResource* wr = nullptr;
    char vdesc[208] = {};
    if (is_view) {
        if (WrapView* v = AsWrapView(iface)) {
            wr = v->wres;
            if (v->kind == 't') {
                const D3D11_SHADER_RESOURCE_VIEW_DESC& vd = static_cast<WrapSRV*>(v)->desc;
                if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
                    _snprintf_s(vdesc, sizeof(vdesc), _TRUNCATE, "SRV buf fmt=%u first=%u nelem=%u",
                        (unsigned)vd.Format, (unsigned)vd.Buffer.FirstElement, (unsigned)vd.Buffer.NumElements);
                else if (!vd.ViewDimension && !vd.Format)
                    _snprintf_s(vdesc, sizeof(vdesc), _TRUNCATE,
                        "SRV NULL-DESC (default full-resource view -- legal, not a fault)");
                else
                    _snprintf_s(vdesc, sizeof(vdesc), _TRUNCATE, "SRV dim=%u fmt=%u", (unsigned)vd.ViewDimension,
                        (unsigned)vd.Format);
            } else if (v->kind == 'u') {
                const D3D11_UNORDERED_ACCESS_VIEW_DESC& vd = static_cast<WrapUAV*>(v)->desc;
                if (vd.ViewDimension == D3D11_UAV_DIMENSION_BUFFER)
                    _snprintf_s(vdesc, sizeof(vdesc), _TRUNCATE, "UAV buf fmt=%u first=%u nelem=%u flags=%u",
                        (unsigned)vd.Format, (unsigned)vd.Buffer.FirstElement, (unsigned)vd.Buffer.NumElements,
                        (unsigned)vd.Buffer.Flags);
                else
                    _snprintf_s(vdesc, sizeof(vdesc), _TRUNCATE, "UAV dim=%u fmt=%u", (unsigned)vd.ViewDimension,
                        (unsigned)vd.Format);
            }
        }
    } else {
        wr = AsWrapResource(iface);
    }
    if (wr && vdesc[0]) {
        const unsigned obs = wr->foreign ? ObservedResourceState(wr->dx12) : kResStateUnknown;
        char flg[48] = "";
        if (wr->dx12) {
            const UINT f = (UINT)wr->dx12->GetDesc().Flags;
            _snprintf_s(flg, sizeof(flg), _TRUNCATE, " flags=0x%x%s", f,
                (f & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? "" : " NO-UAV-FLAG");
        }
        char own[80];
        if (obs != kResStateUnknown)
            _snprintf_s(
                own, sizeof(own), _TRUNCATE, " | GAME-owned state=0x%x observed=0x%x", (unsigned)wr->state, obs);
        else
            _snprintf_s(own, sizeof(own), _TRUNCATE, " | %s state=0x%x",
                wr->foreign ? "GAME-owned (never observed)" : "ours", (unsigned)wr->state);
        strncat_s(vdesc, sizeof(vdesc), own, _TRUNCATE);
        if (flg[0]) strncat_s(vdesc, sizeof(vdesc), flg, _TRUNCATE);
    }
    if (!wr) {
        skip("bound, but not one of our wrappers -- cannot resolve to a DX12 resource");
        return;
    }
    if (!wr->dx12) {
        skip("wrapper has no DX12 resource");
        return;
    }

    ID3D12Resource* src = wr->dx12;
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    const bool is_buffer = (sd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

    UINT64 off = 0;
    UINT64 bytes = 0;
    UINT64 src_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    if (is_buffer) {
        if (wr->gpu_va) { // a WINDOW into a bigger game allocation
            const UINT64 base = src->GetGPUVirtualAddress();
            if (wr->gpu_va >= base) off = wr->gpu_va - base;
        }
        if (off >= sd.Width) {
            skip("window offset past the end of the resource");
            return;
        }
        bytes = sd.Width - off;
        src_size = bytes;
        if (bytes > kSkCapPerResource) bytes = kSkCapPerResource;
    } else {
        UINT rows = 0;
        UINT64 rowbytes = 0;
        UINT64 total = 0;
        dev->GetCopyableFootprints(&sd, 0, 1, 0, &fp, &rows, &rowbytes, &total);
        if (!total) {
            skip("GetCopyableFootprints returned 0 bytes");
            return;
        }
        bytes = src_size = total;
        if (bytes > kSkCapPerResource) {
            skip("texture larger than the per-resource cap");
            return;
        }
    }

    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        if (bytes > g_skcap_budget) {
            logger::write("SKCAP  f=%u seq=%u %-8s %s SKIPPED -- THIS FRAME's staging slice is exhausted "
                          "(%llu B needed, %llu left). Only this frame's bytes are short; later frames get "
                          "their own slice and the per-frame ledger is unaffected. One shapekey chain "
                          "measures 142 MB, so a frame running two of them will always overrun a 10-frame "
                          "slice -- capture fewer frames for full bytes, or read the ledger instead.",
                g_skcap_frame.load(std::memory_order_relaxed), seq, name, post ? "post" : "pre ",
                (unsigned long long)bytes, (unsigned long long)g_skcap_budget);
            return;
        }
        g_skcap_budget -= bytes;
    }

    ID3D12Resource* staging = SkCapMakeReadback(dev, bytes);
    if (!staging) {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        g_skcap_budget += bytes;
        logger::write("SKCAP  seq=%u %-8s staging alloc FAILED (%llu B)", seq, name, (unsigned long long)bytes);
        return;
    }

    const auto copy = [&] {
        if (is_buffer) {
            cl->CopyBufferRegion(staging, 0, src, off, bytes);
        } else {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = staging;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION sl{};
            sl.pResource = src;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &sl, nullptr);
        }
    };
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    src->GetHeapProperties(&hp, &hf);
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT)
        CopyAsSource(cl, src, ListStateOf(cl, wr), copy);
    else
        copy();

    std::lock_guard<std::mutex> l(g_skcap_mtx);
    g_skcap.push_back(SkCapSlot{ 'M', {}, name, post, staging, bytes, src_size, off, (const void*)src,
        (unsigned long long)(src->GetGPUVirtualAddress() + off), seq, cs, gx, gy, gz });
    g_skcap.back().frame = g_skcap_frame.load(std::memory_order_relaxed);
    if (vdesc[0]) memcpy(g_skcap.back().view, vdesc, sizeof(vdesc));
}

void SkCapSnapAll(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, bool post, const void* cs, unsigned seq, UINT x,
    UINT y, UINT z, const std::vector<ShaderReg>* regs)
{
    PipeState& st = State();
    char nm[24];
    const bool declared_only = g_skcap_mode.load(std::memory_order_relaxed) == 1 && regs && !regs->empty();
    auto declared = [&](char kind, int reg) {
        if (!declared_only) return true;
        for (const ShaderReg& r : *regs)
            if (r.kind == kind && r.reg == (UINT)reg) return true;
        return false;
    };
    for (int i = 0; i < kMaxUAV; ++i)
        if (st.uav[i] && declared('u', i)) {
            sprintf_s(nm, "u%d", i);
            SkCapSnap(dev, cl, nm, st.uav[i], true, post, cs, seq, x, y, z);
        }
    for (int i = 0; i < kMaxSRV; ++i)
        if (st.srv[5][i] && declared('t', i)) {
            sprintf_s(nm, "cs-t%d", i);
            SkCapSnap(dev, cl, nm, st.srv[5][i], true, post, cs, seq, x, y, z);
        }
    for (int i = 0; i < kMaxCB; ++i)
        if (st.cb[5][i] && declared('b', i)) {
            sprintf_s(nm, "cs-cb%d", i);
            SkCapSnap(dev, cl, nm, st.cb[5][i], false, post, cs, seq, x, y, z);
        }
}

long long SkCapMapCount(SkCapSlot& e, std::vector<uint8_t>& out)
{
    if (!e.staging) return -1;
    void* p = nullptr;
    D3D12_RANGE rr{ 0, (SIZE_T)e.bytes };
    if (FAILED(e.staging->Map(0, &rr, &p)) || !p) return -1;
    out.assign((const uint8_t*)p, (const uint8_t*)p + (size_t)e.bytes);
    D3D12_RANGE wr0{ 0, 0 };
    e.staging->Unmap(0, &wr0);
    long long nz = 0;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i]) ++nz;
    return nz;
}

void SkCapFlushFrameLocked(unsigned f)
{
    struct Loaded {
        size_t idx;
        std::vector<uint8_t> b;
        long long nz;
    };
    std::vector<Loaded> loaded;
    for (size_t i = 0; i < g_skcap.size(); ++i) {
        SkCapSlot& e = g_skcap[i];
        if (e.done || e.frame != f || !e.staging) continue;
        std::vector<uint8_t> b;
        const long long nz = SkCapMapCount(e, b);
        loaded.push_back(Loaded{ i, std::move(b), nz });
    }
    if (loaded.empty()) return;

    char line[640];
    for (auto& L : loaded) {
        SkCapSlot& e = g_skcap[L.idx];

        if (L.nz < 0) {
            sprintf_s(line, " %c f=%-2u %3u %-9s %-4s %10llu %9llu       MAP FAILED -- no conclusion\n", e.owner,
                e.frame, e.seq, e.name.c_str(), e.post ? "post" : "pre", (unsigned long long)e.bytes,
                (unsigned long long)e.src_size);
            e.digest = line;
        } else {
            const size_t n = L.b.size() / 4;
            const uint32_t* w = (const uint32_t*)L.b.data();
            std::vector<uint32_t> vals;
            size_t nzw = 0;
            size_t comp[6] = { 0, 0, 0, 0, 0, 0 };
            const unsigned long long fnv = Fnv1a64Words(w, n);
            for (size_t i = 0; i < n; ++i) {
                if (!w[i]) continue;
                ++nzw;
                ++comp[i % 6];
                if (vals.size() < (1u << 21)) vals.push_back(w[i]); // bounded; p50 marked partial below
            }
            unsigned long long p50 = 0;
            unsigned long long mx = 0;
            double fp50 = 0.0;
            if (!vals.empty()) {
                std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
                p50 = vals[vals.size() / 2];
                for (uint32_t v : vals)
                    if (v > mx) mx = v;
                float fv;
                const uint32_t bits = (uint32_t)p50;
                memcpy(&fv, &bits, 4);
                fp50 = (double)fv;
            }
            sprintf_s(line,
                " %c f=%-2u %3u %-9s %-4s %10llu %9llu %8zu %5.1f%%  u32 p50=%llu max=%llu"
                "  asfloat(p50)=%.6g fnv=%016llx%s\n",
                e.owner, e.frame, e.seq, e.name.c_str(), e.post ? "post" : "pre", (unsigned long long)e.bytes,
                (unsigned long long)e.src_size, nzw, n ? 100.0 * (double)nzw / (double)n : 0.0, p50, mx, fp50, fnv,
                vals.size() >= (1u << 21) ? "  (p50 over first 2M non-zero)" : "");
            e.digest = line;

            if (nzw) {
                int k = sprintf_s(line, "        comp6={");
                bool first = true;
                for (int c = 0; c < 6; ++c)
                    if (comp[c]) {
                        k += sprintf_s(line + k, sizeof(line) - k, "%s%d:%zu", first ? "" : ",", c, comp[c]);
                        first = false;
                    }
                sprintf_s(line + k, sizeof(line) - k, "}\n");
                e.digest += line;
            }

            if (e.post) {
                for (auto& P : loaded) {
                    SkCapSlot& q = g_skcap[P.idx];
                    if (q.post || q.seq != e.seq || q.name != e.name || P.nz < 0) continue;
                    const size_t m = (P.b.size() < L.b.size() ? P.b.size() : L.b.size()) / 4;
                    const uint32_t* a = (const uint32_t*)P.b.data();
                    size_t changed = 0;
                    double maxd = 0.0;
                    for (size_t i = 0; i < m; ++i) {
                        if (a[i] == w[i]) continue;
                        ++changed;
                        float x;
                        float y;
                        memcpy(&x, &a[i], 4);
                        memcpy(&y, &w[i], 4);
                        const double d = (double)y - (double)x;
                        const double ad = d < 0 ? -d : d;
                        if (ad == ad && ad > maxd) maxd = ad; // NaN-safe: a delta that is not a
                    } // number cannot be the maximum
                    sprintf_s(line, "        vs pre: %zu dword(s) changed of %zu, max |float delta| = %.6g%s\n",
                        changed, m, maxd, changed ? "" : "   <- THIS PASS WROTE NOTHING TO THIS SLOT");
                    e.digest += line;
                    break;
                }
            }
        }

        if (e.view[0]) {
            char v[288];
            sprintf_s(v, "        view= %s\n", e.view);
            e.digest += v;
        }
        if (e.prov[0]) {
            char v[128];
            sprintf_s(v, "        prov= %s\n", e.prov);
            e.digest += v;
        }
    }

    for (auto& L : loaded) {
        SkCapSlot& e = g_skcap[L.idx];
        if (e.bytes && e.bytes <= kSkCapFullWrite) e.keep = std::move(L.b);
        if (e.staging) {
            e.staging->Release();
            e.staging = nullptr;
        }
        e.done = true;
    }
}

bool SkCapWrite(const std::wstring& dir, const std::wstring& leaf, const void* data, size_t bytes)
{
    std::wstring path = dir + L"\\" + leaf;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = (bytes == 0) || (fwrite(data, 1, bytes, f) == bytes);
    fclose(f);
    return ok;
}

} // namespace

bool SkCapArmed()
{
    return g_skcap_arm.load(std::memory_order_relaxed) > 0;
}

struct SkCapBail {
    const void* cs;
    UINT gx;
    UINT gy;
    UINT gz;
    unsigned long tid;
    unsigned depth;
    bool had_dev;
    unsigned frame;
};
static std::mutex g_skcap_bail_mtx;
static std::vector<SkCapBail> g_skcap_bails;

void SkCapRecordBail(const void* cs, UINT x, UINT y, UINT z, bool had_device)
{
    if (g_skcap_arm.load(std::memory_order_relaxed) <= 0) return;
    SkCapBail e{ cs, x, y, z, (unsigned long)GetCurrentThreadId(), ::PortApi::BridgeStackDepth(), had_device,
        g_skcap_frame.load(std::memory_order_relaxed) };
    std::lock_guard<std::mutex> l(g_skcap_bail_mtx);
    if (g_skcap_bails.size() < 512) g_skcap_bails.push_back(e);
}

static void SkCapStageRaw(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const char* name, ID3D12Resource* src,
    UINT64 off, UINT64 bytes, UINT state, const void* cs, unsigned seq, UINT gx, UINT gy, UINT gz, const char* prov,
    char owner, bool counts_as_dispatch)
{
    if (!dev || !cl || !src || !bytes || !name) return;
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return; // textures need a placed footprint
    if (off >= sd.Width) return;
    UINT64 avail = sd.Width - off;
    if (bytes > avail) bytes = avail;
    if (bytes > kSkCapPerResource) bytes = kSkCapPerResource;
    if (strstr(name, "-cb") && bytes > 65536ull) bytes = 65536ull;

    // X12 rule 2: in FRAME mode a GAME row spends the G rows' own share of the slice, never the M/D rows'.
    const bool game_share = (owner == 'G') && g_skcap_mode.load(std::memory_order_relaxed) == 1;
    UINT64& budget = game_share ? g_skcap_game_budget : g_skcap_budget;
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        if (bytes > budget) {
            logger::write("SKCAP  seq=%u %-8s %s SKIPPED -- %s exhausted", seq, name, owner == 'D' ? "DRAW" : "GAME",
                game_share ? "the GAME rows' share of this frame's slice (X12)" : "staging budget");
            return;
        }
        budget -= bytes;
    }

    ID3D12Resource* staging = SkCapMakeReadback(dev, bytes);
    if (!staging) {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        budget += bytes;
        return;
    }

    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    src->GetHeapProperties(&hp, &hf);
    const auto copy = [&] { cl->CopyBufferRegion(staging, 0, src, off, bytes); };
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT)
        CopyAsSource(cl, src, (D3D12_RESOURCE_STATES)state, copy);
    else
        copy();

    if (counts_as_dispatch) {
        static std::atomic<unsigned> s_last_seq{ 0xFFFFFFFFu };
        if (s_last_seq.exchange(seq) != seq) {
            if (g_skcap_mode.load(std::memory_order_relaxed) == 0 && g_skcap_arm.load(std::memory_order_relaxed) > 0)
                g_skcap_arm.fetch_sub(1, std::memory_order_relaxed);
            g_skcap_wait.store(3);
        }
    } else {
        g_skcap_wait.store(3); // a draw row still needs the readback to settle before the drain
    }

    SkCapSlot slot;
    slot.owner = owner;
    if (prov && *prov) {
        size_t n = strlen(prov);
        if (n >= sizeof(slot.prov)) n = sizeof(slot.prov) - 1;
        memcpy(slot.prov, prov, n);
        slot.prov[n] = 0;
    }
    slot.name = name;
    slot.post = false; // pre-only: no post-dispatch event exists
    slot.staging = staging;
    slot.bytes = bytes;
    slot.src_size = sd.Width;
    slot.src_off = off;
    slot.src_res = src;
    slot.src_va = src->GetGPUVirtualAddress() + off;
    slot.seq = seq;
    slot.cs = cs;
    slot.gx = gx;
    slot.gy = gy;
    slot.gz = gz;
    slot.frame = g_skcap_frame.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        g_skcap.push_back(slot);
    }
}

void SkCapSnapRaw(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const char* name, ID3D12Resource* src, UINT64 off,
    UINT64 bytes, UINT state, const void* cs, unsigned seq, UINT gx, UINT gy, UINT gz, const char* prov)
{
    SkCapStageRaw(dev, cl, name, src, off, bytes, state, cs, seq, gx, gy, gz, prov, 'G', true);
}

void SkCapSnapDraw(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const char* name, ID3D12Resource* src, UINT64 off,
    UINT64 bytes, UINT state, unsigned seq, const char* prov)
{
    if (g_skcap_mode.load(std::memory_order_relaxed) == 1) {
        const unsigned f = g_skcap_frame.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        for (const SkCapDSeen& s : g_skcap_d_seen)
            if (s.frame == f && s.cl == (const void*)cl && s.res == (const void*)src && s.off == off) {
                ++g_skcap_d_dedup;
                return;
            }
        g_skcap_d_seen.push_back(SkCapDSeen{ f, (const void*)cl, (const void*)src, off });
    }
    SkCapStageRaw(dev, cl, name, src, off, bytes, state, nullptr, seq, 0, 0, 0, prov, 'D', false);
}

// Shared reset. Mode-specific fields are set by the two entry points below.
static void SkCapResetLocked(UINT64 budget)
{
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        for (auto& e : g_skcap)
            if (e.staging) e.staging->Release();
        g_skcap.clear();
        g_skcap_skip.clear();
        g_skcap_pass.clear();
        g_skcap_seq = 0;
        g_skcap_budget = budget;
        g_skcap_game_budget = 0;
        g_skcap_d_seen.clear();
        g_skcap_d_dedup = 0;
    }
    {
        std::lock_guard<std::mutex> l(g_skcap_bail_mtx);
        g_skcap_bails.clear();
    }
    g_skcap_wait.store(0);
    g_skcap_frame.store(0);
}

void SkCapSetCaller(const wchar_t* ns, unsigned long long cs, unsigned tgy, int mod)
{
    std::lock_guard<std::mutex> l(g_skcap_caller_mtx);
    g_skcap_caller_cs = cs;
    g_skcap_caller_tgy = tgy;
    g_skcap_caller_mod = mod;
    g_skcap_caller_ns[0] = 0;
    if (ns) {
        wcsncpy_s(g_skcap_caller_ns, ns, _TRUNCATE);
    }
}

void SkCapArm(int dispatches)
{
    if (dispatches <= 0) dispatches = 1;
    SkCapResetLocked(kSkCapTotalBudget);
    g_skcap_mode.store(0);
    g_skcap_frames_armed.store(0);
    g_skcap_frames_left.store(0);
    g_skcap_arm.store(dispatches);
    logger::write("SKCAP armed for %d DISPATCH(es), FULL BYTES -- PRE and POST snapshots of every bound "
                  "compute slot written to disk, plus CS bytecode and the shader's reflected register "
                  "declarations. Budget %llu MB; one shapekey chain measures 146 MB. Records carry a "
                  "frame stamp, but this mode cannot span frames -- use FRAME mode for that.",
        dispatches, (unsigned long long)(kSkCapTotalBudget >> 20));
}

void SkCapArmFrames(int frames)
{
    if (frames <= 0) frames = 1;
    const UINT64 slice = kSkCapTotalBudget / (UINT64)frames;
    SkCapResetLocked(slice);
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx); // X12 rule 2: split the first slice too
        g_skcap_game_budget = g_skcap_budget / kSkCapGameShareDiv;
        g_skcap_budget -= g_skcap_game_budget;
    }
    g_skcap_mode.store(1);
    g_skcap_frames_armed.store(frames);
    g_skcap_frames_left.store(frames);
    g_skcap_arm.store(1); // a flag in this mode -- the frame tick ends the capture
    logger::write("SKCAP armed for %d FRAME(s), DIGEST -- every compute dispatch on those frames. Each "
                  "record carries the frame it was taken on, so 'the chain did not run this frame' is a "
                  "recorded fact rather than an absence. Full .buf only for slots <= %llu KB; larger "
                  "slots are reduced to non-zero count, p50 over non-zero values, the index%%6 component "
                  "histogram and a post-vs-pre changed-dword count -- every statistic this chain's "
                  "diagnoses have actually rested on. Staging %llu MB sliced to %llu MB per frame, freed "
                  "3 frames after capture so peak stays at one frame.",
        frames, (unsigned long long)(kSkCapFullWrite >> 10), (unsigned long long)(kSkCapTotalBudget >> 20),
        (unsigned long long)(slice >> 20));
}

void SkCapBeforeDispatch(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z)
{
    if (g_skcap_arm.load(std::memory_order_relaxed) <= 0) return;

    const void* cs = (const void*)State().shader[5];
    unsigned seq;
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        seq = g_skcap_seq;
    }

    SkCapPass p;
    p.seq = seq;
    p.cs = cs;
    p.gx = x;
    p.gy = y;
    p.gz = z;
    p.frame = g_skcap_frame.load(std::memory_order_relaxed);
    p.t_begin_us = SkCapNowUs();
    {
        std::lock_guard<std::mutex> l(g_skcap_caller_mtx);
        p.caller_cs = g_skcap_caller_cs;
        p.caller_tgy = g_skcap_caller_tgy;
        p.caller_mod = g_skcap_caller_mod;
        wcsncpy_s(p.caller_ns, g_skcap_caller_ns, _TRUNCATE);
    }
    ShaderCode* sc = AsShaderCode(State().shader[5]);
    if (sc) {
        p.code = sc->code;
        p.regs = sc->regs;
    }
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        if (g_skcap_pass.size() < 4096) g_skcap_pass.push_back(std::move(p));
    }

    SkCapSnapAll(dev, cl, /*post=*/false, cs, seq, x, y, z, sc ? &sc->regs : nullptr);
}

void SkCapAfterDispatch(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z)
{
    if (g_skcap_arm.load(std::memory_order_relaxed) <= 0) return;
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        if (!g_skcap_pass.empty()) g_skcap_pass.back().t_end_us = SkCapNowUs();
    }
    if (g_skcap_mode.load(std::memory_order_relaxed) == 0 && g_skcap_arm.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return;

    const void* cs = (const void*)State().shader[5];
    unsigned seq;
    {
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        seq = g_skcap_seq++;
    }

    // The CS is still the bound one, so its reflected declarations are the same list Before recorded.
    ShaderCode* sc = AsShaderCode(State().shader[5]);
    SkCapSnapAll(dev, cl, /*post=*/true, cs, seq, x, y, z, sc ? &sc->regs : nullptr);
    {
        const uint32_t mask = g_cs_uav_bound_mask.exchange(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> l(g_skcap_mtx);
        for (auto it = g_skcap_pass.rbegin(); it != g_skcap_pass.rend(); ++it)
            if (it->seq == seq) {
                it->uav_bound_mask = mask;
                break;
            }
    }

    g_skcap_wait.store(3);
}

void SkCapDrain()
{
    if (g_skcap_frames_left.load(std::memory_order_relaxed) > 0) {
        if (g_skcap_frames_left.fetch_sub(1, std::memory_order_relaxed) <= 1) {
            g_skcap_arm.store(0, std::memory_order_relaxed); // stop capturing
            g_skcap_wait.store(3); // then let the readback settle
        } else {
            const unsigned cur = g_skcap_frame.fetch_add(1, std::memory_order_relaxed) + 1;
            std::lock_guard<std::mutex> l(g_skcap_mtx);
            if (cur >= 3) SkCapFlushFrameLocked(cur - 3);
            g_skcap_budget = kSkCapTotalBudget /
                (UINT64)(g_skcap_frames_armed.load(std::memory_order_relaxed) > 0
                        ? g_skcap_frames_armed.load(std::memory_order_relaxed)
                        : 1);
            // X12: the G rows' share comes out of the slice; the D-row dedupe set is per frame.
            g_skcap_game_budget = g_skcap_budget / kSkCapGameShareDiv;
            g_skcap_budget -= g_skcap_game_budget;
            g_skcap_d_seen.clear();
        }
        return; // never drain mid-capture
    }

    const int w = g_skcap_wait.load(std::memory_order_relaxed);
    if (w <= 0) return;
    if (g_skcap_wait.fetch_sub(1, std::memory_order_relaxed) > 1) return;

    std::lock_guard<std::mutex> l(g_skcap_mtx);
    if (g_skcap.empty() && g_skcap_pass.empty()) return;

    // One directory per capture, beside every other diagnostic output.
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(
        stamp, L"SKCAP-%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring dir = (DumpDir().empty() ? std::wstring(L".") : DumpDir()) + L"\\" + stamp;
    CreateDirectoryW(dir.c_str(), nullptr);

    std::string man;
    man.reserve(1 << 16);
    char line[1024];
    const int armed = g_skcap_frames_armed.load(std::memory_order_relaxed);
    sprintf_s(line,
        "SKCAP capture -- PRE/POST snapshots taken around each Dispatch, copies recorded into\n"
        "the GAME's command list so POST bytes are genuinely after the dispatch executed.\n"
        "%d frame(s) armed, %zu pass(es), %zu staged slot(s).\n\n",
        armed, g_skcap_pass.size(), g_skcap.size());
    man += line;

    {
        size_t nf = (size_t)(armed > 0 ? armed : 1);
        for (auto& p : g_skcap_pass)
            if ((size_t)p.frame + 1 > nf) nf = (size_t)p.frame + 1;
        for (auto& e : g_skcap)
            if ((size_t)e.frame + 1 > nf) nf = (size_t)e.frame + 1;
        std::vector<unsigned> pf(nf, 0);
        std::vector<unsigned> bf(pf.size(), 0);
        std::vector<unsigned> sf(pf.size(), 0);
        for (auto& p : g_skcap_pass)
            if (p.frame < pf.size()) ++pf[p.frame];
        for (auto& e : g_skcap)
            if (e.frame < sf.size()) ++sf[e.frame];
        {
            std::lock_guard<std::mutex> bail_lock(g_skcap_bail_mtx);
            for (auto& e : g_skcap_bails)
                if (e.frame < bf.size()) ++bf[e.frame];
        }
        man += "PER-FRAME LEDGER  (passes = dispatches we injected, refused = dispatches we could not)\n";
        man += "  frame  passes  refused  staged-slots\n";
        for (size_t i = 0; i < pf.size(); ++i) {
            sprintf_s(line, "  %5zu  %6u  %7u  %12u%s\n", i, pf[i], bf[i], sf[i],
                (pf[i] == 0 && bf[i] == 0) ? "   <- no compute dispatch seen at all"
                                           : (pf[i] == 0 ? "   <- CHAIN DID NOT RUN THIS FRAME" : ""));
            man += line;
        }
        if (g_skcap_mode.load(std::memory_order_relaxed) == 1) {
            sprintf_s(line,
                "  X12: passes staged only their DECLARED registers; GAME rows had 1/%llu of each slice;\n"
                "  %u D row(s) not staged because the same list had already staged that slot-6 resource\n"
                "  this frame (a list's later draws read what its first draw read).\n",
                (unsigned long long)kSkCapGameShareDiv, g_skcap_d_dedup);
            man += line;
        }
        man += "\n";
    }

    // ---- the passes: bytecode + what the shader DECLARES -----------------------------------------
    for (auto& p : g_skcap_pass) {
        {
            char ub[96] = "none";
            if (p.uav_bound_mask) {
                int n = 0;
                for (int r = 0; r < 16; ++r)
                    if (p.uav_bound_mask & (1u << r)) n += sprintf_s(ub + n, sizeof(ub) - n, "%su%d", n ? "," : "", r);
            }
            sprintf_s(line,
                "    BOUND compute UAV registers this dispatch: %s   (what actually reached a "
                "root parameter -- if the register the shader writes is missing here, the "
                "write goes nowhere)\n",
                ub);
            man += line;
        }
        char cl2[256];
        _sprintf_s_l(cl2, sizeof(cl2), "    t=%llu..%lluus (%lluus)  caller: cs=%016llx tgy=%u mod=%d ns='%ls'\n",
            logger::utf8_locale(), p.t_begin_us, p.t_end_us,
            p.t_end_us > p.t_begin_us ? p.t_end_us - p.t_begin_us : 0ull, p.caller_cs, p.caller_tgy, p.caller_mod,
            p.caller_ns);
        man += cl2;
        sprintf_s(line, "PASS f=%u seq=%u  cs=%p  groups=%u,%u,%u  bytecode=%zu B\n", p.frame, p.seq, p.cs, p.gx, p.gy,
            p.gz, p.code.size());
        man += line;
        if (!p.code.empty()) {
            wchar_t leaf[64];
            swprintf_s(leaf, L"%03u_cs.cso", p.seq);
            if (!SkCapWrite(dir, leaf, p.code.data(), p.code.size())) man += "    (bytecode write FAILED)\n";
        } else {
            man += "    (no bytecode -- AsShaderCode returned nothing for this dispatch)\n";
        }
        man += "    declares:";
        if (p.regs.empty()) man += " (not reflected)";
        for (auto& r : p.regs) {
            sprintf_s(line, " %c%u", r.kind, r.reg);
            man += line;
        }
        man += "\n";
    }
    man += "\n";

    // ---- the slots: pre vs post, so "did this pass write it" is a fact, not a guess --------------
    {
        std::lock_guard<std::mutex> bail_lock(g_skcap_bail_mtx);
        if (!g_skcap_bails.empty()) {
            sprintf_s(line,
                "REFUSED DISPATCHES: %zu -- recorded at the `!b.cl || !b.device` gate, which sits\n"
                "  BEFORE the capture hook, so these never appear as PASS rows. depth>0 with cl=null\n"
                "  means a draw scope IS open somewhere and this thread is not seeing it.\n",
                g_skcap_bails.size());
            man += line;
            size_t shown = 0;
            for (auto& e : g_skcap_bails) {
                if (shown++ >= 64) break; // ten armed frames refuse far more than 16
                sprintf_s(line, "  BAIL f=%u cs=%p groups=%u,%u,%u thread=%lu bridgeStackDepth=%u device=%s\n", e.frame,
                    e.cs, e.gx, e.gy, e.gz, e.tid, e.depth, e.had_dev ? "yes" : "NO");
                man += line;
            }
            g_skcap_bails.clear();
        }
    }
    {
        sprintf_s(line, "CHAIN CONFIG -- their parsed values. allow_buffer_resize(G) = %s\n",
            ::PortApi::AllowBufferResize() ? "TRUE" : "FALSE");
        man += line;
        man +=
            "  (their parser gates the whole resize block on that global, IniHandler.cpp parse_texture_override_common)\n";
        man += "  section                                             hash      byte_width  num_elem  prio\n";
        struct Ctx {
            std::string* out;
            size_t shown;
        };
        Ctx ctx{ &man, 0 };
        ::PortApi::ForEachTextureOverrideConfig(
            [](unsigned hash, int bw, int ne, int prio, const wchar_t* section, void* user) {
                Ctx* c = (Ctx*)user;
                std::wstring low = section ? section : L"";
                for (wchar_t& ch : low) ch = (wchar_t)towlower(ch);
                const bool interesting = bw > 0 || low.find(L"shapekey") != std::wstring::npos ||
                    low.find(L"position") != std::wstring::npos;
                if (!interesting || c->shown >= 400) return;
                ++c->shown;
                char l2[320];
                _sprintf_s_l(l2, sizeof(l2), "  %-50ls %08x  %10d  %8d  %4d\n", logger::utf8_locale(),
                    section ? section : L"(unnamed)", hash, bw, ne, prio);
                *c->out += l2;
            },
            &ctx);
        if (!ctx.shown) man += "  (no shapekey/position sections and no resizes in their map)\n";
        man += "\n";
    }

    man += "own = M (our replayed chain, PRE+POST) or G (the GAME's own dispatch, PRE only --\n"
           "      there is no post-dispatch event, so a G row is an INPUT sample). An unmodded\n"
           "      character gives only G rows, and those are the REFERENCE: the same buffers,\n"
           "      computed correctly, same machine and same API. G vs M is a diff.\n"
           "      D (I9) = the widened buffer as the INPUT ASSEMBLER reads it through slot 6 at a\n"
           "      claimed draw: staged on the draw's own list after the UAV->VB barrier and the rebind.\n"
           "      prov= names the draw: f=frame m=mod c=comp l=list d=depth-only x=barrier-emitted\n"
           "      n=index_count fi=first_index. fnv= is FNV-1a over the dwords: a D row whose fnv equals\n"
           "      this frame's last u0 post row reads THIS frame's chain output; an earlier frame's,\n"
           "      a STALE one; none, something the chain never produced.\n"
           "      Constant-buffer rows (...-cbN) are staged 64 KB at most (X6): a CBV cannot read past\n"
           "      that, and staging the game's 32 MB constant ring per pass starved the budget.\n";
    {
        man += "HASH RESOLUTION -- their key is the whole D3D11_BUFFER_DESC; we reconstruct it.\n";
        man += "  bytes        assumed   searched  note\n";
        std::vector<unsigned> sizes;
        for (auto& e : g_skcap) {
            if (!e.bytes || e.bytes > 0xFFFFFFFFull) continue;
            const unsigned bw = (unsigned)e.src_size;
            bool have = false;
            for (unsigned x : sizes)
                if (x == bw) {
                    have = true;
                    break;
                }
            if (!have) sizes.push_back(bw);
        }
        size_t shown = 0;
        for (unsigned bw : sizes) {
            if (shown++ >= 24) break;
            bool amb = false;
            const unsigned assumed = ::PortApi::GameComputeUavBufferHash(bw);
            const unsigned searched = ::PortApi::GameBufferHashSearch(bw, 0, &amb);
            const char* note = amb      ? "AMBIGUOUS -- several declared hashes reachable"
                : (!searched)           ? "no declared hash matches any plausible desc"
                : (searched == assumed) ? "agree"
                                        : "DISAGREE -- the port names this buffer wrongly";
            sprintf_s(line, "  %-11u  %08x  %08x  %s\n", bw, assumed, searched, note);
            man += line;
        }
        man += "\n";
    }

    size_t files = 0;
    if (g_skcap_mode.load(std::memory_order_relaxed) == 1) {
        for (int ff = 0; ff <= armed; ++ff) SkCapFlushFrameLocked((unsigned)ff);

        man += "SLOT DIGESTS. Full .buf files are written only for slots <= 1 MB; everything larger is\n"
               "reduced to the statistics below, which are the ones every conclusion about this chain has\n"
               "actually rested on (non-zero count, p50 over the NON-ZERO values, index%6 component\n"
               "histogram, and post-vs-pre changed-dword count). Use DISPATCH mode (G4) when the bytes\n"
               "themselves are the question.\n";
        man += "own f    seq slot      dir     copied_B     src_B  nonzero    pct  stats\n";
        for (auto& e : g_skcap) {
            man += e.digest;
            if (!e.keep.empty()) {
                wchar_t leaf[96];
                swprintf_s(leaf, L"%03u_f%u_%hs.%hs.buf", e.seq, e.frame, e.name.c_str(), e.post ? "post" : "pre");
                if (SkCapWrite(dir, leaf, e.keep.data(), e.keep.size()))
                    ++files;
                else
                    man += "        (write FAILED)\n";
            }
        }
    } else {
        man += "own seq slot      dir   copied_B     src_B   src_off        gpu_va  nonzero   pct  provenance / note\n";
        man += "  (a row with no view= is a RAW resource capture, not a bound view -- absence is not a fault)\n";
        for (auto& e : g_skcap) {
            std::vector<uint8_t> bytes;
            const long long nz = SkCapMapCount(e, bytes);
            if (nz < 0) {
                sprintf_s(line, " %c  %3u %-9s %-4s %10llu %9llu %9llu %13llx      MAP FAILED -- no conclusion\n",
                    e.owner, e.seq, e.name.c_str(), e.post ? "post" : "pre", (unsigned long long)e.bytes,
                    (unsigned long long)e.src_size, (unsigned long long)e.src_off, e.src_va);
                man += line;
            } else {
                wchar_t leaf[96];
                swprintf_s(leaf, L"%03u_%hs.%hs.buf", e.seq, e.name.c_str(), e.post ? "post" : "pre");
                const bool wrote = SkCapWrite(dir, leaf, bytes.data(), bytes.size());
                if (wrote) ++files;
                // Same identity as the FRAME-mode digest, so a D row and an M row match by eye here too.
                const unsigned long long fnv = Fnv1a64Words((const uint32_t*)bytes.data(), bytes.size() / 4);
                sprintf_s(line, " %c  %3u %-9s %-4s %10llu %9llu %9llu %13llx %8lld %5.1f  fnv=%016llx  %s%s\n",
                    e.owner, e.seq, e.name.c_str(), e.post ? "post" : "pre", (unsigned long long)e.bytes,
                    (unsigned long long)e.src_size, (unsigned long long)e.src_off, e.src_va, nz,
                    e.bytes ? 100.0 * (double)nz / (double)e.bytes : 0.0, fnv,
                    e.prov[0] ? e.prov : (nz == 0 ? "ALL ZERO " : ""), wrote ? "" : "(write FAILED)");
                man += line;
                if (e.view[0]) {
                    sprintf_s(line, "        view= %s\n", e.view);
                } else {
                    line[0] = 0;
                }
                man += line;
            }
            if (e.staging) {
                e.staging->Release();
                e.staging = nullptr;
            }
        }
    }

    man += "\n";
    if (g_skcap_skip.empty()) {
        man += "bound-but-not-staged: none. Every bound compute slot above was captured.\n";
    } else {
        man += "BOUND BUT NOT STAGED -- these were bound and are NOT in the table above:\n";
        for (auto& s : g_skcap_skip) {
            sprintf_s(line, "%3u %-9s %-4s  %s\n", s.seq, s.name.c_str(), s.post ? "post" : "pre", s.why.c_str());
            man += line;
        }
    }

    if (!SkCapWrite(dir, L"manifest.txt", man.data(), man.size())) logger::write("SKCAP: manifest write FAILED");

    char narrow[512];
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
    logger::write("SKCAP wrote %zu slot file(s) (of %zu slots -- the rest are digested in the manifest) "
                  "+ %zu shader(s) + manifest.txt -> %s",
        files, g_skcap.size(), g_skcap_pass.size(), narrow);

    g_skcap.clear();
    g_skcap_skip.clear();
    g_skcap_pass.clear();
}

} // namespace MigotoPort
