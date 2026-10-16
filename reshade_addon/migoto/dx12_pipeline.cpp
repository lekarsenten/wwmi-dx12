// ===========================================================================
//  dx12_pipeline.cpp -- OURS. PORT-LEAF. DX11 pipeline state -> DX12 PSO, root signature, descriptors.
//
//  See dx12_pipeline.h for why this is a translation rather than an emulation. Everything here is
//  derived, never hand-tabulated: the root signature comes from REFLECTING the shader bytecode their
//  parser handed us, so a mod can bind any register it likes and there is no per-shader table to go
//  stale.
// ===========================================================================
#include "dx12_pipeline.h"
#include "../instrumentation/tex_timing.h"
#include "dx12_wrap.h"
#include "dx12_context.h"
#include "port_toggles.h" // g_blend_write_all_rt, g_inherit_game_viewport, g_uav_barrier
#include "port_keygate.h"
#include "../../src/gpu_progress.h"
#include "../../src/log.h"
#include "../../src/fnv.h"
#include "../instrumentation/removal_pin.h" // the per-list tally of what this file records (REMOVAL-PIN)
#include "../instrumentation/gpu_timer.h"   // GPU ms of what this file records (performance monitor)
#include "../../src/d3d12_buffer.h"
#include "../../src/d3d12_barrier.h"
#include "prof_core.h" // PROF(PL_*), ProfGpuWork: IssueDraw/IssueDispatch and the upload path
#include "dx12_pipeline_internal.h"

// Frame analysis, for the COMPUTE half -- see the call at the end of IssueDispatch.
#include "frame_analysis_driver.h"
#include "DrawCallInfo.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace MigotoPort {

bool (*g_query_reloc)(unsigned long long, unsigned long long*, unsigned long long*) = nullptr;

namespace {

std::mutex& Mtx()
{
    static std::mutex* m = new std::mutex();
    return *m;
}

enum : UINT {
    kStagingSize = 4096,
    kRingSize = 8192,
    kRtvSize = 256,
    kDsvSize = 64,
    kSampStaging = 256,
    kSampRing = 1024,
};

struct Heap {
    ID3D12DescriptorHeap*              heap = nullptr;
    UINT                               inc = 0;
    UINT                               size = 0;
    UINT                               next = 0; // ring cursor
    std::vector<UINT>                  freed;    // staging free list
    std::vector<uint64_t>              tag;      // ring: per 256-slot block, GpuStamp() at its last write
    std::vector<ID3D12DescriptorHeap*> more;     // staging: further blocks of `size` slots once the first is used up
    D3D12_CPU_DESCRIPTOR_HANDLE        cpu0{};
    D3D12_GPU_DESCRIPTOR_HANDLE        gpu0{};

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) const
    {
        if (i < size) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpu0;
            h.ptr += (SIZE_T)i * inc;
            return h;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h = more[i / size - 1]->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(i % size) * inc;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT i) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = gpu0;
        h.ptr += (UINT64)i * inc;
        return h;
    }
};

struct RootBind {
    UINT          param;
    char          stage;
    char          kind;
    UINT          reg;
    UINT          space;
    unsigned char sit = 0;
    unsigned char dim = 0;
};

struct RootSigEntry {
    ID3D12RootSignature*  rs = nullptr;
    std::vector<RootBind> binds;
};

struct Heaps {
    Heap                                               staging;
    Heap                                               ring;
    Heap                                               rtv;
    Heap                                               dsv;
    Heap                                               samp_staging;
    Heap                                               samp_ring;
    std::unordered_map<uint64_t, RootSigEntry>         rootsigs;
    std::unordered_map<uint64_t, ID3D12PipelineState*> psos;
    ID3D12Device*                                      dev = nullptr;
};
Heaps& H()
{
    static Heaps* h = new Heaps();
    return *h;
}

bool MakeHeap(Heap& h, ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shader_visible)
{
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = type;
    d.NumDescriptors = count;
    d.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                             : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h.heap)))) return false;
    h.inc = dev->GetDescriptorHandleIncrementSize(type);
    h.size = count;
    h.next = 0;
    h.cpu0 = h.heap->GetCPUDescriptorHandleForHeapStart();
    if (shader_visible) h.gpu0 = h.heap->GetGPUDescriptorHandleForHeapStart();
    return true;
}

Heap* StagingFor(char kind)
{
    switch (kind) {
    case 't':
    case 'u':
    case 'b': return &H().staging;
    case 'r': return &H().rtv;
    case 'd': return &H().dsv;
    case 's': return &H().samp_staging;
    }
    return nullptr;
}

D3D12_BLEND Blend(D3D11_BLEND b)
{
    switch (b) {
    case D3D11_BLEND_ZERO: return D3D12_BLEND_ZERO;
    case D3D11_BLEND_ONE: return D3D12_BLEND_ONE;
    case D3D11_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_COLOR;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_COLOR;
    case D3D11_BLEND_SRC_ALPHA: return D3D12_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_ALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_ALPHA: return D3D12_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_ALPHA: return D3D12_BLEND_INV_DEST_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_COLOR;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_COLOR;
    case D3D11_BLEND_SRC_ALPHA_SAT: return D3D12_BLEND_SRC_ALPHA_SAT;
    case D3D11_BLEND_BLEND_FACTOR: return D3D12_BLEND_BLEND_FACTOR;
    case D3D11_BLEND_INV_BLEND_FACTOR: return D3D12_BLEND_INV_BLEND_FACTOR;
    case D3D11_BLEND_SRC1_COLOR: return D3D12_BLEND_SRC1_COLOR;
    case D3D11_BLEND_INV_SRC1_COLOR: return D3D12_BLEND_INV_SRC1_COLOR;
    case D3D11_BLEND_SRC1_ALPHA: return D3D12_BLEND_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_ALPHA: return D3D12_BLEND_INV_SRC1_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP BlendOp(D3D11_BLEND_OP o)
{
    switch (o) {
    case D3D11_BLEND_OP_ADD: return D3D12_BLEND_OP_ADD;
    case D3D11_BLEND_OP_SUBTRACT: return D3D12_BLEND_OP_SUBTRACT;
    case D3D11_BLEND_OP_REV_SUBTRACT: return D3D12_BLEND_OP_REV_SUBTRACT;
    case D3D11_BLEND_OP_MIN: return D3D12_BLEND_OP_MIN;
    case D3D11_BLEND_OP_MAX: return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

D3D12_STENCIL_OP StencilOp(D3D11_STENCIL_OP o)
{
    switch (o) {
    case D3D11_STENCIL_OP_KEEP: return D3D12_STENCIL_OP_KEEP;
    case D3D11_STENCIL_OP_ZERO: return D3D12_STENCIL_OP_ZERO;
    case D3D11_STENCIL_OP_REPLACE: return D3D12_STENCIL_OP_REPLACE;
    case D3D11_STENCIL_OP_INCR_SAT: return D3D12_STENCIL_OP_INCR_SAT;
    case D3D11_STENCIL_OP_DECR_SAT: return D3D12_STENCIL_OP_DECR_SAT;
    case D3D11_STENCIL_OP_INVERT: return D3D12_STENCIL_OP_INVERT;
    case D3D11_STENCIL_OP_INCR: return D3D12_STENCIL_OP_INCR;
    case D3D11_STENCIL_OP_DECR: return D3D12_STENCIL_OP_DECR;
    }
    return D3D12_STENCIL_OP_KEEP;
}

void FillBlend(D3D12_BLEND_DESC& out, const D3D11_BLEND_DESC* in)
{
    if (!in) { // DX11's default: no blending, write all channels
        const UINT8 mask = D3D12_COLOR_WRITE_ENABLE_ALL;
        out.RenderTarget[0].RenderTargetWriteMask = mask;
        if (g_blend_write_all_rt)
            for (int i = 1; i < 8; ++i) out.RenderTarget[i].RenderTargetWriteMask = mask;
        return;
    }
    out.AlphaToCoverageEnable = in->AlphaToCoverageEnable ? TRUE : FALSE;
    out.IndependentBlendEnable = in->IndependentBlendEnable ? TRUE : FALSE;
    for (int i = 0; i < 8; ++i) {
        const D3D11_RENDER_TARGET_BLEND_DESC& s = in->RenderTarget[i];
        D3D12_RENDER_TARGET_BLEND_DESC&       d = out.RenderTarget[i];
        d.BlendEnable = s.BlendEnable ? TRUE : FALSE;
        d.LogicOpEnable = FALSE;
        d.SrcBlend = Blend(s.SrcBlend);
        d.DestBlend = Blend(s.DestBlend);
        d.BlendOp = BlendOp(s.BlendOp);
        d.SrcBlendAlpha = Blend(s.SrcBlendAlpha);
        d.DestBlendAlpha = Blend(s.DestBlendAlpha);
        d.BlendOpAlpha = BlendOp(s.BlendOpAlpha);
        d.LogicOp = D3D12_LOGIC_OP_NOOP;
        d.RenderTargetWriteMask = s.RenderTargetWriteMask;
    }
}

void FillRaster(D3D12_RASTERIZER_DESC& out, const D3D11_RASTERIZER_DESC* in)
{
    // DX11's default rasteriser state, which is what a [CustomShader] with no `cull`/`fill` gets.
    out.FillMode = D3D12_FILL_MODE_SOLID;
    out.CullMode = D3D12_CULL_MODE_BACK;
    out.FrontCounterClockwise = FALSE;
    out.DepthClipEnable = TRUE;
    out.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    if (!in) return;
    out.FillMode = (in->FillMode == D3D11_FILL_WIREFRAME) ? D3D12_FILL_MODE_WIREFRAME
                                                          : D3D12_FILL_MODE_SOLID;
    out.CullMode = (in->CullMode == D3D11_CULL_NONE) ? D3D12_CULL_MODE_NONE
        : (in->CullMode == D3D11_CULL_FRONT)         ? D3D12_CULL_MODE_FRONT
                                                     : D3D12_CULL_MODE_BACK;
    out.FrontCounterClockwise = in->FrontCounterClockwise ? TRUE : FALSE;
    out.DepthBias = in->DepthBias;
    out.DepthBiasClamp = in->DepthBiasClamp;
    out.SlopeScaledDepthBias = in->SlopeScaledDepthBias;
    out.DepthClipEnable = in->DepthClipEnable ? TRUE : FALSE;
    out.MultisampleEnable = in->MultisampleEnable ? TRUE : FALSE;
    out.AntialiasedLineEnable = in->AntialiasedLineEnable ? TRUE : FALSE;
}

void FillDepth(D3D12_DEPTH_STENCIL_DESC& out, const D3D11_DEPTH_STENCIL_DESC* in, bool have_dsv)
{
    if (!in) {
        out.DepthEnable = have_dsv ? TRUE : FALSE;
        out.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        out.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        return;
    }
    out.DepthEnable = (in->DepthEnable && have_dsv) ? TRUE : FALSE;
    out.DepthWriteMask = (in->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO)
        ? D3D12_DEPTH_WRITE_MASK_ZERO
        : D3D12_DEPTH_WRITE_MASK_ALL;
    out.DepthFunc = (D3D12_COMPARISON_FUNC)in->DepthFunc;
    out.StencilEnable = (in->StencilEnable && have_dsv) ? TRUE : FALSE;
    out.StencilReadMask = in->StencilReadMask;
    out.StencilWriteMask = in->StencilWriteMask;
    out.FrontFace.StencilFailOp = StencilOp(in->FrontFace.StencilFailOp);
    out.FrontFace.StencilDepthFailOp = StencilOp(in->FrontFace.StencilDepthFailOp);
    out.FrontFace.StencilPassOp = StencilOp(in->FrontFace.StencilPassOp);
    out.FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)in->FrontFace.StencilFunc;
    out.BackFace.StencilFailOp = StencilOp(in->BackFace.StencilFailOp);
    out.BackFace.StencilDepthFailOp = StencilOp(in->BackFace.StencilDepthFailOp);
    out.BackFace.StencilPassOp = StencilOp(in->BackFace.StencilPassOp);
    out.BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)in->BackFace.StencilFunc;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopoType(D3D11_PRIMITIVE_TOPOLOGY t)
{
    switch (t) {
    case D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default:
        if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_SHADER_VISIBILITY Visibility(char stage)
{
    switch (stage) {
    case 'v': return D3D12_SHADER_VISIBILITY_VERTEX;
    case 'h': return D3D12_SHADER_VISIBILITY_HULL;
    case 'd': return D3D12_SHADER_VISIBILITY_DOMAIN;
    case 'g': return D3D12_SHADER_VISIBILITY_GEOMETRY;
    case 'p': return D3D12_SHADER_VISIBILITY_PIXEL;
    }
    return D3D12_SHADER_VISIBILITY_ALL; // compute signatures must use ALL
}

bool BuildRootSig(const std::vector<RootBind>& want, bool compute, RootSigEntry& out)
{
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(want.size());
    std::vector<D3D12_ROOT_PARAMETER>   params(want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        D3D12_DESCRIPTOR_RANGE& r = ranges[i];
        r.RangeType = (want[i].kind == 'u') ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
            : (want[i].kind == 'b')         ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
            : (want[i].kind == 's')         ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                                            : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = want[i].reg;
        r.RegisterSpace = want[i].space;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER& p = params[i];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = { 1, &ranges[i] };
        p.ShaderVisibility = compute ? D3D12_SHADER_VISIBILITY_ALL : Visibility(want[i].stage);
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = (UINT)params.size();
    rsd.pParameters = params.empty() ? nullptr : params.data();
    rsd.Flags = compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                        : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT   hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
        logger::write("PORT: root signature serialise FAILED hr=%08lX %s", (unsigned long)hr,
            err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    hr = H().dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&out.rs));
    blob->Release();
    if (err) err->Release();
    if (FAILED(hr)) {
        logger::write("PORT: CreateRootSignature FAILED hr=%08lX (%zu params)", (unsigned long)hr,
            want.size());
        return false;
    }
    out.binds = want;
    return true;
}

RootSigEntry* GetRootSig(const std::vector<RootBind>& want, bool compute)
{
    uint64_t key = Fnv1a64(want.empty() ? "" : (const void*)want.data(),
        want.size() * sizeof(RootBind));
    key = Fnv1a64(&compute, sizeof(compute), key);
    auto it = H().rootsigs.find(key);
    if (it != H().rootsigs.end()) return it->second.rs ? &it->second : nullptr;
    RootSigEntry& e = H().rootsigs[key];
    if (!BuildRootSig(want, compute, e)) return nullptr;
    logger::write("PORT: root signature built -- %zu tables, %s", want.size(),
        compute ? "compute" : "graphics");
    return &e;
}

// A texture in COMMON takes the copy and shader-read states on a list with no barrier.
static bool Promotable(const WrapResource* r, D3D12_RESOURCE_STATES to)
{
    if (r->state != D3D12_RESOURCE_STATE_COMMON) return false;
    const D3D12_RESOURCE_STATES tex = D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return to != D3D12_RESOURCE_STATE_COMMON && (to & tex) == to;
}

using ListBufStates = std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>>;
std::mutex                                                    g_list_buf_mtx;
std::unordered_set<ID3D12Resource*>                           g_owned_bufs;
std::unordered_map<ID3D12GraphicsCommandList*, ListBufStates> g_list_buf;
std::unordered_map<ID3D12Resource*, uint64_t>                 g_owned_writes;
std::unordered_map<ID3D12GraphicsCommandList*, uint64_t>      g_list_recording;
uint64_t                                                      g_list_recordings = 0;
std::atomic<uint64_t>                                         g_upload_seq{ 0 };

uint64_t ListRecording(ID3D12GraphicsCommandList* cl)
{
    if (!cl) return 0;
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    auto                        it = g_list_recording.find(cl);
    return it == g_list_recording.end() ? 0 : it->second;
}

void UavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res)
{
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = res;
    cl->ResourceBarrier(1, &ub);
    ProfGpuWork(PGW_UavBarrier, 0);
    RemovalPin::Ours(cl, "port-uav-barrier");
}

class OwedUavBarriers {
public:
    void Note(ID3D12GraphicsCommandList* cl, ID3D12Resource* res)
    {
        std::lock_guard<std::mutex>   l(mtx_);
        std::vector<ID3D12Resource*>& owed = lists_[cl];
        if (std::find(owed.begin(), owed.end(), res) == owed.end()) owed.push_back(res);
    }

    bool Take(ID3D12GraphicsCommandList* cl, ID3D12Resource* res)
    {
        std::lock_guard<std::mutex> l(mtx_);
        auto                        it = lists_.find(cl);
        if (it == lists_.end()) return false;
        std::vector<ID3D12Resource*>& owed = it->second;
        auto                          r = std::find(owed.begin(), owed.end(), res);
        if (r == owed.end()) return false;
        *r = owed.back();
        owed.pop_back();
        if (owed.empty()) lists_.erase(it);
        return true;
    }

    void Pay(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, bool transitioned)
    {
        if (transitioned)
            ProfGpuWork(PGW_UavCovered, 0);
        else
            UavBarrier(cl, res);
    }

    void Flush(ID3D12GraphicsCommandList* cl)
    {
        std::vector<ID3D12Resource*> owed;
        {
            std::lock_guard<std::mutex> l(mtx_);
            auto                        it = lists_.find(cl);
            if (it == lists_.end()) return;
            owed.swap(it->second);
            lists_.erase(it);
        }
        std::vector<D3D12_RESOURCE_BARRIER> ub(owed.size());
        for (size_t i = 0; i < owed.size(); ++i) {
            ub[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            ub[i].UAV.pResource = owed[i];
            ProfGpuWork(PGW_UavBarrier, 0);
        }
        cl->ResourceBarrier((UINT)ub.size(), ub.data());
        RemovalPin::Ours(cl, "port-uav-barrier");
    }

    void Forget(ID3D12GraphicsCommandList* cl)
    {
        std::lock_guard<std::mutex> l(mtx_);
        lists_.erase(cl);
    }

private:
    std::mutex                                                                     mtx_;
    std::unordered_map<ID3D12GraphicsCommandList*, std::vector<ID3D12Resource*>> lists_;
};
OwedUavBarriers g_owed_uav;

bool HostHeap(WrapResource* r)
{
    if (r->heap_type == (D3D12_HEAP_TYPE)0) {
        D3D12_HEAP_PROPERTIES hp{};
        D3D12_HEAP_FLAGS      hf{};
        // Fails for reserved/tiled resources, which have no heap; DEFAULT keeps them transitionable.
        r->heap_type = SUCCEEDED(r->dx12->GetHeapProperties(&hp, &hf)) ? hp.Type : D3D12_HEAP_TYPE_DEFAULT;
    }
    return r->heap_type == D3D12_HEAP_TYPE_UPLOAD || r->heap_type == D3D12_HEAP_TYPE_READBACK;
}

bool BufferRead(D3D12_RESOURCE_STATES s)
{
    return s != D3D12_RESOURCE_STATE_COMMON && (s & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0;
}

struct ListBufChange {
    D3D12_RESOURCE_STATES was = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES now = D3D12_RESOURCE_STATE_COMMON;

    static D3D12_RESOURCE_STATES Next(D3D12_RESOURCE_STATES was, D3D12_RESOURCE_STATES to)
    {
        if (!BufferRead(to) || was == D3D12_RESOURCE_STATE_COMMON) return to;
        if (BufferRead(was) && (was & to) == to) return was;
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    }

    bool Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES to) const
    {
        if (now == was) {
            if (now != to) ProfGpuWork(PGW_ReadKept, 0);
            return false;
        }
        if (was == D3D12_RESOURCE_STATE_COMMON) {
            ProfGpuWork(PGW_Promoted, 0);
            return false;
        }
        TransitionBarrier(cl, res, was, now);
        ProfGpuWork(PGW_Barrier, 0);
        RemovalPin::Ours(cl, "port-transition");
        return true;
    }
};

ListBufChange SwapListBufState(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES to)
{
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    ListBufStates&              ls = g_list_buf[cl];
    ListBufChange               c;
    for (auto& e : ls)
        if (e.first == res) {
            c.was = e.second;
            c.now = ListBufChange::Next(c.was, to);
            e.second = c.now;
            return c;
        }
    c.now = to;
    ls.emplace_back(res, to);
    return c;
}

bool StateBarrier(ID3D12GraphicsCommandList* cl, WrapResource* r, D3D12_RESOURCE_STATES to)
{
    if (r->foreign) {
        const uint32_t obs = ObservedResourceState(r->dx12);
        if (obs != kResStateUnknown) r->state = (D3D12_RESOURCE_STATES)obs;
    }

    if (r->state == to) return false;
    if (!r->foreign && Promotable(r, to)) {
        r->state = to;
        ProfGpuWork(PGW_Promoted, 0);
        return false;
    }

    if (HostHeap(r)) return false;

    TransitionBarrier(cl, r->dx12, r->state, to);
    ProfGpuWork(PGW_Barrier, 0);
    RemovalPin::Ours(cl, "port-transition");
    r->state = to;
    return true;
}

void Transition(ID3D12GraphicsCommandList* cl, WrapResource* r, D3D12_RESOURCE_STATES to)
{
    if (!cl || !r || !r->dx12) return;

    if (r->dim == D3D11_RESOURCE_DIMENSION_BUFFER && OwnedBuffer(r->dx12)) {
        OwnedTransition(cl, r->dx12, to);
        r->state = to;
        return;
    }

    const bool owed = g_owed_uav.Take(cl, r->dx12);
    const bool transitioned = StateBarrier(cl, r, to);
    if (owed) g_owed_uav.Pay(cl, r->dx12, transitioned);
}

void WriteNullDescriptor(char kind, D3D12_CPU_DESCRIPTOR_HANDLE dst,
    unsigned char sit, unsigned char dim)
{
    if (kind == 'b') {
        D3D12_CONSTANT_BUFFER_VIEW_DESC d{};
        H().dev->CreateConstantBufferView(&d, dst);
    } else if (kind == 'u') {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        switch (sit) {
        case D3D_SIT_UAV_RWBYTEADDRESS:
            d.Format = DXGI_FORMAT_R32_TYPELESS; // raw REQUIRES typeless
            d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            break;
        case D3D_SIT_UAV_RWSTRUCTURED:
        case D3D_SIT_UAV_APPEND_STRUCTURED:
        case D3D_SIT_UAV_CONSUME_STRUCTURED:
        case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
            d.Format = DXGI_FORMAT_UNKNOWN; // structured: stride, not format
            break;
        default:
            d.Format = DXGI_FORMAT_R32_UINT; // RWTYPED, and the unrecorded case
            break;
        }
        H().dev->CreateUnorderedAccessView(nullptr, nullptr, &d, dst);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (sit == D3D_SIT_BYTEADDRESS) {
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        } else if (sit == D3D_SIT_STRUCTURED) {
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Format = DXGI_FORMAT_UNKNOWN;
        } else if (dim == D3D_SRV_DIMENSION_BUFFER) {
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Format = DXGI_FORMAT_R32_UINT;
        } else {
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels = 1;
        }
        H().dev->CreateShaderResourceView(nullptr, &d, dst);
    }
}

bool CbvWindow(ID3D11Buffer* cb, D3D12_GPU_VIRTUAL_ADDRESS* out_va, UINT64* out_size, bool* out_windowed)
{
    WrapResource* wr = AsWrapResource(cb);
    if (!wr || !wr->dx12) return false;
    const D3D12_RESOURCE_DESC       rd = wr->dx12->GetDesc();
    const D3D12_GPU_VIRTUAL_ADDRESS base = wr->dx12->GetGPUVirtualAddress();

    D3D12_GPU_VIRTUAL_ADDRESS va = base;
    UINT64                    size = rd.Width;
    bool                      windowed = false;
    if (wr->gpu_va && wr->gpu_va >= base && wr->gpu_va < base + rd.Width) {
        windowed = true;
        va = wr->gpu_va;
        const UINT64      remaining = rd.Width - (wr->gpu_va - base);
        D3D11_BUFFER_DESC bd{};
        ((ID3D11Buffer*)cb)->GetDesc(&bd);
        const UINT64 want = bd.ByteWidth ? bd.ByteWidth : remaining;
        size = (want < remaining) ? want : remaining;
    }
    const UINT64 kMaxCbv = 65536; // D3D12's hard CBV limit
    if (size > kMaxCbv) size = kMaxCbv;

    if (out_va) *out_va = va;
    if (out_size) *out_size = size;
    if (out_windowed) *out_windowed = windowed;
    return true;
}

void WriteCbvDescriptor(ID3D11Buffer* cb, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    UINT64                    size = 0;
    if (!CbvWindow(cb, &va, &size, nullptr)) {
        WriteNullDescriptor('b', dst, 0, 0);
        return;
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC d{};
    d.BufferLocation = va;
    d.SizeInBytes = (UINT)((size + 255) & ~(UINT64)255); // DX12 requires a 256-byte multiple
    H().dev->CreateConstantBufferView(&d, dst);
}

struct PendingUpload {
    ID3D11Resource*                                 dst = nullptr;     // our wrapper; we hold a ref until the copy is recorded
    ID3D12Resource*                                 staging = nullptr; // a private staging buffer, or the ring (from_ring)
    bool                                            is_texture = false;
    bool                                            from_ring = false; // source is the upload ring: nothing to retire or release
    UINT                                            subresources = 1;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;        // ABSOLUTE offsets into `staging`
    UINT64                                          src_offset = 0; // buffer uploads: where in `staging` the bytes start
    UINT64                                          bytes = 0;
    uint64_t                                        queued_stamp = 0;
    uint64_t                                        seq = 0;
};
std::vector<PendingUpload>& Uploads()
{
    static auto* v = new std::vector<PendingUpload>();
    return *v;
}
struct RetiredBuf {
    ID3D12Resource* res;
    uint64_t        stamp;
    ResourceAllocation memory;
};
std::vector<RetiredBuf>& RetiredStaging()
{
    static auto* v = new std::vector<RetiredBuf>();
    return *v;
}
struct UploadRing {
    ID3D12Resource*       res = nullptr;
    uint8_t*              mapped = nullptr; // persistently mapped: legal for an UPLOAD heap
    UINT64                size = 0;
    UINT64                head = 0;
    size_t                cur_chunk = SIZE_MAX; // the chunk the head is in; SIZE_MAX = none yet
    std::vector<uint64_t> chunk_tag;            // per chunk: GpuStamp() at the last write; 0 = never
    bool                  failed = false;       // creation failed once: do not retry on every write
};
constexpr UINT64 kRingBytes = 8ull << 20; // 8 MB: ~24 writes x ~16 KB per frame x the frames in flight, with room
constexpr UINT64 kRingChunk = 64ull << 10;
constexpr UINT64 kRingAlign = 512;           // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
constexpr UINT64 kRingMaxWrite = 1ull << 20; // above this a write keeps its own staging buffer
UploadRing&      Ring()
{
    static auto* r = new UploadRing();
    return *r;
}
ID3D12Resource* MakeStaging(UINT64 bytes);

// Reserve `bytes` in the ring. Caller holds Mtx(). False = the ring cannot serve this write now.
bool RingAlloc(UINT64 bytes, UINT64* out_offset)
{
    UploadRing& r = Ring();
    if (r.failed || bytes == 0 || bytes > kRingMaxWrite) return false;
    if (!r.res) {
        r.res = MakeStaging(kRingBytes);
        if (!r.res) {
            r.failed = true;
            return false;
        }
        D3D12_RANGE none{ 0, 0 };
        void*       p = nullptr;
        if (FAILED(r.res->Map(0, &none, &p)) || !p) {
            r.res->Release();
            r.res = nullptr;
            r.failed = true;
            logger::write("PORT: upload ring Map FAILED -- every upload keeps its own staging buffer");
            return false;
        }
        r.mapped = static_cast<uint8_t*>(p);
        r.size = kRingBytes;
        r.chunk_tag.assign((size_t)(kRingBytes / kRingChunk), 0u);
        logger::write("PORT: upload ring mapped: %llu MB in %zu chunks of %llu KB",
            (unsigned long long)(kRingBytes >> 20), r.chunk_tag.size(),
            (unsigned long long)(kRingChunk >> 10));
    }
    UINT64 off = (r.head + (kRingAlign - 1)) & ~(kRingAlign - 1);
    if (off + bytes > r.size) off = 0; // wrap
    const uint64_t now_tag = GpuStamp();
    const size_t   c0 = (size_t)(off / kRingChunk);
    const size_t   c1 = (size_t)((off + bytes - 1) / kRingChunk);
    for (size_t c = c0; c <= c1; ++c) {
        if (c == r.cur_chunk && off != 0) continue; // continuing in the head's own chunk
        const uint64_t t = r.chunk_tag[c];
        if (t != 0 && !GpuPassed(t)) return false; // last lap's copy may be in flight
    }
    for (size_t c = c0; c <= c1; ++c) r.chunk_tag[c] = now_tag;
    r.cur_chunk = c1;
    r.head = off + bytes;
    *out_offset = off;
    return true;
}

void RingTouch(UINT64 off, UINT64 bytes)
{
    UploadRing& r = Ring();
    if (!r.res || bytes == 0) return;
    const uint64_t now_tag = GpuStamp();
    const size_t   c0 = (size_t)(off / kRingChunk);
    const size_t   c1 = (size_t)((off + bytes - 1) / kRingChunk);
    for (size_t c = c0; c <= c1 && c < r.chunk_tag.size(); ++c) r.chunk_tag[c] = now_tag;
}

std::atomic<unsigned long long> g_ring_expired{ 0 };

} // namespace

namespace {
thread_local ModDrawFn g_mod_draw_fn = nullptr;
thread_local void*     g_mod_draw_user = nullptr;
} // namespace

void SetModDrawCallback(ModDrawFn fn, void* user)
{
    g_mod_draw_fn = fn;
    g_mod_draw_user = user;
}

ModDrawFn ModDrawCallback(void** out_user)
{
    if (out_user) *out_user = g_mod_draw_user;
    return g_mod_draw_fn;
}

namespace {
struct CbSnapshot {
    ID3D12Resource* res = nullptr;
    uint64_t        stamp = 0;
};
struct CbSnapshotUse {
    ID3D12Resource*           src;
    UINT64                    src_off;
    UINT                      bytes;
    uint64_t                  writes;
    D3D12_GPU_VIRTUAL_ADDRESS va;
};
constexpr UINT64                                                           kCbSnapshotBytes = 64 * 1024;
constexpr UINT64                                                           kCbSnapshotsPerHeap = 64;
std::mutex                                                                 g_cb_snap_mtx;
std::vector<CbSnapshot>                                                    g_cb_snaps;
std::vector<ID3D12Heap*>                                                   g_cb_snap_heaps;
std::unordered_map<ID3D12GraphicsCommandList*, std::vector<CbSnapshotUse>> g_cb_snap_uses;
std::atomic<unsigned>                                                      g_cb_snap_logged{ 0 };

uint64_t OwnedWrites(ID3D12Resource* res)
{
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    auto                        it = g_owned_writes.find(res);
    return it == g_owned_writes.end() ? 0 : it->second;
}

void ForgetCbSnapshotUses(ID3D12GraphicsCommandList* cl)
{
    std::lock_guard<std::mutex> l(g_cb_snap_mtx);
    g_cb_snap_uses.erase(cl);
}

ID3D12Resource* TakeCbSnapshot()
{
    for (CbSnapshot& s : g_cb_snaps)
        if (GpuPassed(s.stamp)) {
            s.stamp = GpuStamp();
            return s.res;
        }
    return nullptr;
}

bool GrowCbSnapshots(ID3D12Device* dev)
{
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = kCbSnapshotBytes * kCbSnapshotsPerHeap;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ID3D12Heap* heap = nullptr;
    if (FAILED(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap))) || !heap) return false;
    g_cb_snap_heaps.push_back(heap);
    const size_t before = g_cb_snaps.size();
    for (UINT64 i = 0; i < kCbSnapshotsPerHeap; ++i) {
        ID3D12Resource* res = nullptr;
        if (FAILED(CreatePlacedD3D12Buffer(dev, heap, i * kCbSnapshotBytes, kCbSnapshotBytes, D3D12_RESOURCE_STATE_COMMON, &res)) || !res)
            break;
        g_cb_snaps.push_back({ res, 0 });
    }
    return g_cb_snaps.size() > before;
}
} // namespace

void OwnBuffer(ID3D12Resource* res)
{
    if (!res) return;
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    g_owned_bufs.insert(res);
}

void DisownBuffer(ID3D12Resource* res)
{
    if (!res) return;
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    g_owned_bufs.erase(res);
    g_owned_writes.erase(res);
}

bool OwnedBuffer(ID3D12Resource* res)
{
    if (!res) return false;
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    return g_owned_bufs.count(res) != 0;
}

D3D12_RESOURCE_STATES OwnedTransition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES to)
{
    if (!cl || !res) return D3D12_RESOURCE_STATE_COMMON;
    if (to & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        std::lock_guard<std::mutex> l(g_list_buf_mtx);
        ++g_owned_writes[res];
    }
    const bool          owed = g_owed_uav.Take(cl, res);
    const ListBufChange c = SwapListBufState(cl, res, to);
    const bool          transitioned = c.Record(cl, res, to);
    if (owed) g_owed_uav.Pay(cl, res, transitioned);
    return c.was;
}

D3D12_RESOURCE_STATES ListStateOf(ID3D12GraphicsCommandList* cl, WrapResource* r)
{
    if (!r) return D3D12_RESOURCE_STATE_COMMON;
    if (!cl || !r->dx12 || r->dim != D3D11_RESOURCE_DIMENSION_BUFFER || !OwnedBuffer(r->dx12)) return r->state;
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    auto                        it = g_list_buf.find(cl);
    if (it == g_list_buf.end()) return D3D12_RESOURCE_STATE_COMMON;
    for (const auto& e : it->second)
        if (e.first == r->dx12) return e.second;
    return D3D12_RESOURCE_STATE_COMMON;
}

void FlushUavBarriers(ID3D12GraphicsCommandList* cl)
{
    if (cl) g_owed_uav.Flush(cl);
}

void ListBuffersForget(ID3D12GraphicsCommandList* cl)
{
    if (!cl) return;
    g_owed_uav.Forget(cl);
    ForgetCbSnapshotUses(cl);
    std::lock_guard<std::mutex> l(g_list_buf_mtx);
    g_list_buf.erase(cl);
    g_list_recording[cl] = ++g_list_recordings;
}

void ListBuffersToCommon(ID3D12GraphicsCommandList* cl)
{
    if (!cl) return;
    g_owed_uav.Flush(cl);
    ForgetCbSnapshotUses(cl);
    ListBufStates st;
    {
        std::lock_guard<std::mutex> l(g_list_buf_mtx);
        g_list_recording.erase(cl);
        auto                        it = g_list_buf.find(cl);
        if (it == g_list_buf.end()) return;
        st.swap(it->second);
        g_list_buf.erase(it);
    }
    bool any = false;
    for (const auto& [res, s] : st)
        if (s != D3D12_RESOURCE_STATE_COMMON) {
            TransitionBarrier(cl, res, s, D3D12_RESOURCE_STATE_COMMON);
            ProfGpuWork(PGW_Barrier, 0);
            any = true;
        }
    if (any) RemovalPin::Ours(cl, "port-list-close");
}

D3D12_GPU_VIRTUAL_ADDRESS StaticCbSnapshot(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, UINT64 src_off, UINT bytes)
{
    ID3D12Device* dev = GetDx12Bridge().device;
    if (!cl || !src || !dev || !bytes) return 0;
    if (bytes > kCbSnapshotBytes) bytes = (UINT)kCbSnapshotBytes;
    const uint64_t  writes = OwnedWrites(src);
    ID3D12Resource* snap = nullptr;
    size_t          pool = 0;
    {
        std::lock_guard<std::mutex> l(g_cb_snap_mtx);
        for (const CbSnapshotUse& u : g_cb_snap_uses[cl])
            if (u.src == src && u.src_off == src_off && u.bytes == bytes && u.writes == writes) return u.va;
        snap = TakeCbSnapshot();
        if (!snap && GrowCbSnapshots(dev)) {
            pool = g_cb_snaps.size();
            snap = TakeCbSnapshot();
        }
        if (!snap) return 0;
    }
    if (pool && g_cb_snap_logged.fetch_add(1, std::memory_order_relaxed) < 16)
        logger::write("CBSNAP: %zu snapshot buffer(s) of %llu KB -- a static root CBV of the game reads one of ours", pool,
            (unsigned long long)(kCbSnapshotBytes / 1024));
    OwnedTransition(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(snap, 0, src, src_off, bytes);
    TransitionBarrier(cl, snap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    ProfGpuWork(PGW_ShimCopy, bytes);
    RemovalPin::Ours(cl, "port-cb-snapshot");
    const D3D12_GPU_VIRTUAL_ADDRESS va = snap->GetGPUVirtualAddress();
    {
        std::lock_guard<std::mutex> l(g_cb_snap_mtx);
        g_cb_snap_uses[cl].push_back({ src, src_off, bytes, writes, va });
    }
    return va;
}

void TransitionResource(ID3D12GraphicsCommandList* cl, ID3D11Resource* res, D3D12_RESOURCE_STATES to)
{
    Transition(cl, AsWrapResource(res), to);
}

namespace {
struct BufCopyCs {
    std::vector<unsigned char> dxil;
    ID3D12RootSignature*       rs = nullptr;
    ID3D12PipelineState*       pso = nullptr;
    bool                       failed = false;
    bool                       Ensure(ID3D12Device* dev)
    {
        if (pso) return true;
        if (failed || !dev || dxil.empty()) return false;
        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.Num32BitValues = 4;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t0 = src
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; // u0 = dst
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = rp;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT   hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
        if (blob) blob->Release();
        if (err) err->Release();
        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = rs;
            pd.CS.pShaderBytecode = dxil.data();
            pd.CS.BytecodeLength = dxil.size();
            hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        }
        if (FAILED(hr)) {
            logger::write("BUFCOPY: build FAILED hr=%08lX -- buffer copies stay CopyResource", (unsigned long)hr);
            failed = true;
            return false;
        }
        logger::write("BUFCOPY: ready -- the shim's buffer copies run as a compute dispatch");
        return true;
    }
};
BufCopyCs& BufCopy()
{
    static auto* p = new BufCopyCs();
    return *p;
}
}

void SetBufCopyShader(const void* dxil, size_t bytes)
{
    if (!dxil || !bytes) return;
    BufCopy().dxil.assign((const unsigned char*)dxil, (const unsigned char*)dxil + bytes);
}

bool CopyBufferCs(ID3D12GraphicsCommandList* cl, WrapResource* dst, UINT64 dst_off,
    WrapResource* src, UINT64 src_off, UINT64 bytes)
{
    const Dx12Bridge& b = GetDx12Bridge();
    if (!cl || !dst || !src || !dst->dx12 || !src->dx12 || !bytes || (bytes & 3) || !BufCopy().Ensure(b.device)) return false;
    for (WrapResource* r : { dst, src })
        if (r->heap_type == (D3D12_HEAP_TYPE)0) {
            D3D12_HEAP_PROPERTIES hp{};
            D3D12_HEAP_FLAGS      hf{};
            r->heap_type = SUCCEEDED(r->dx12->GetHeapProperties(&hp, &hf)) ? hp.Type : D3D12_HEAP_TYPE_DEFAULT;
        }
    if (dst->heap_type != D3D12_HEAP_TYPE_DEFAULT || src->heap_type != D3D12_HEAP_TYPE_DEFAULT) return false;
    if (!(dst->dx12->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
    if (dst_off > 0xFFFFFFFFull || src_off > 0xFFFFFFFFull || bytes > 0xFFFFFFFFull) return false;
    Transition(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cl, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t consts[4] = { (uint32_t)src_off, (uint32_t)dst_off, (uint32_t)bytes, 0 };
    cl->SetComputeRootSignature(BufCopy().rs);
    cl->SetPipelineState(BufCopy().pso);
    cl->SetComputeRoot32BitConstants(0, 4, consts, 0);
    cl->SetComputeRootShaderResourceView(1, src->dx12->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(2, dst->dx12->GetGPUVirtualAddress());
    cl->Dispatch((UINT)((bytes + 1023) / 1024), 1, 1);
    RemovalPin::Ours(cl, "port-bufcopy");
    if (g_uav_barrier) g_owed_uav.Note(cl, dst->dx12);
    OweGameState(b);
    return true;
}

namespace {

// Both queue paths need the same UPLOAD-heap buffer.
ID3D12Resource* MakeStaging(UINT64 bytes)
{
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device || !bytes) return nullptr;
    ID3D12Resource* r = nullptr;
    if (FAILED(CreateD3D12Buffer(b.device, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ, &r)))
        return nullptr;
    return r;
}

} // namespace

void QueueUpload(ID3D11Resource* dst, const void* data, size_t bytes, uint64_t seq)
{
    WrapResource* wr = AsWrapResource(dst);
    if (!wr || !wr->dx12 || !data || !bytes) return;
    PROF(PL_UploadQueue);
    PendingUpload u;
    u.bytes = bytes;
    UINT64 off = 0;
    bool   ring = false;
    {
        std::lock_guard<std::mutex> l(Mtx());
        ring = RingAlloc(bytes, &off);
    }
    u.queued_stamp = GpuStamp();
    if (ring) {
        memcpy(Ring().mapped + off, data, bytes);
        u.staging = Ring().res;
        u.src_offset = off;
        u.from_ring = true;
    } else {
        ID3D12Resource* staging = MakeStaging(bytes);
        if (!staging) {
            logger::write("PORT: upload staging FAILED (%zu bytes)", bytes);
            return;
        }
        void*       p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        if (SUCCEEDED(staging->Map(0, &none, &p)) && p) {
            memcpy(p, data, bytes);
            staging->Unmap(0, nullptr);
        }
        u.staging = staging;
    }
    u.dst = dst;
    u.seq = seq;
    dst->AddRef();
    std::lock_guard<std::mutex> l(Mtx());
    wr->queued_seq = seq;
    Uploads().push_back(std::move(u));
}

void QueueTextureUpload(ID3D11Resource* dst, const D3D11_SUBRESOURCE_DATA* data, UINT subresources, uint64_t seq)
{
    {
        static const bool skip = [] {
            char   b[8] = {};
            size_t n = 0;
            return getenv_s(&n, b, sizeof(b), "PORT_NO_TEXUP") == 0 && n && b[0] == '1';
        }();
        if (skip) return;
    }
    WrapResource* wr = AsWrapResource(dst);
    if (!wr || !wr->dx12 || !data || !subresources) return;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device) return;
    PROF(PL_UploadQueue);

    const D3D12_RESOURCE_DESC                       rd = wr->dx12->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources);
    std::vector<UINT>                               rows(subresources);
    std::vector<UINT64>                             row_bytes(subresources);
    UINT64                                          total = 0;
    b.device->GetCopyableFootprints(&rd, 0, subresources, 0, layouts.data(), rows.data(),
        row_bytes.data(), &total);

    if (subresources == 1 && data[0].pSysMem) {
        std::lock_guard<std::mutex> l(Mtx());
        for (auto it = Uploads().rbegin(); it != Uploads().rend(); ++it) {
            if (it->dst != dst || !it->is_texture || !it->from_ring || it->subresources != 1 || it->staging != Ring().res) continue;
            uint8_t*       dst_rows = Ring().mapped + it->layouts[0].Offset;
            const uint8_t* src_rows = (const uint8_t*)data[0].pSysMem;
            for (UINT y = 0; y < rows[0]; ++y)
                memcpy(dst_rows + (size_t)y * it->layouts[0].Footprint.RowPitch,
                    src_rows + (size_t)y * data[0].SysMemPitch, (size_t)row_bytes[0]);
            it->seq = seq;
            wr->queued_seq = seq;
            ProfGpuWork(PGW_UploadDedup, 0);
            return;
        }
    }

    ID3D12Resource* staging = nullptr;
    uint8_t*        base = nullptr; // where layouts[i].Offset is relative to
    UINT64          off = 0;
    bool            ring = false;
    {
        std::lock_guard<std::mutex> l(Mtx());
        ring = RingAlloc(total, &off);
    }
    if (ring) {
        b.device->GetCopyableFootprints(&rd, 0, subresources, off, layouts.data(), rows.data(),
            row_bytes.data(), &total);
        staging = Ring().res;
        base = Ring().mapped; // absolute offsets: base is the ring's start
    } else {
        staging = MakeStaging(total);
        if (!staging) {
            logger::write("PORT: texture upload staging FAILED (%llu bytes)",
                (unsigned long long)total);
            return;
        }
        D3D12_RANGE none{ 0, 0 };
        if (FAILED(staging->Map(0, &none, (void**)&base)) || !base) {
            staging->Release();
            return;
        }
    }
    for (UINT i = 0; i < subresources; ++i) {
        uint8_t*       dst_rows = base + layouts[i].Offset;
        const uint8_t* src_rows = (const uint8_t*)data[i].pSysMem;
        if (!src_rows) continue;
        for (UINT y = 0; y < rows[i]; ++y) {
            memcpy(dst_rows + (size_t)y * layouts[i].Footprint.RowPitch,
                src_rows + (size_t)y * data[i].SysMemPitch,
                (size_t)row_bytes[i]);
        }
    }
    if (!ring) staging->Unmap(0, nullptr);

    PendingUpload u;
    u.dst = dst;
    dst->AddRef();
    u.staging = staging;
    u.from_ring = ring;
    u.is_texture = true;
    u.subresources = subresources;
    u.layouts = std::move(layouts);
    u.bytes = total;
    u.queued_stamp = GpuStamp();
    u.seq = seq;
    std::lock_guard<std::mutex> l(Mtx());
    wr->queued_seq = seq;
    Uploads().push_back(std::move(u));
}

uint64_t NextUploadSeq()
{
    return g_upload_seq.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool ListHoldsUpload(ID3D12GraphicsCommandList* cl, const WrapResource* wr)
{
    if (!cl || !wr || !wr->uploaded_seq) return false;
    const uint64_t recording = ListRecording(cl);
    if (!recording) return false;
    std::lock_guard<std::mutex> l(Mtx());
    return wr->queued_seq == wr->uploaded_seq && wr->flushed.seq == wr->uploaded_seq && wr->flushed.list == cl &&
        wr->flushed.recording == recording;
}

void FlushPendingUploads(ID3D12GraphicsCommandList* cl)
{
    if (!cl) return;
    PROF(PL_UploadFlush);
    const uint64_t             recording = ListRecording(cl);
    std::vector<PendingUpload> todo;
    {
        std::lock_guard<std::mutex> l(Mtx());
        if (Uploads().empty()) return;
        todo.swap(Uploads());
        for (const PendingUpload& u : todo)
            if (WrapResource* wr = AsWrapResource(u.dst)) wr->flushed = { u.seq, cl, recording };
        // Release only what the GPU is demonstrably done with, instead of everything queued last time.
        auto&  ret = RetiredStaging();
        size_t keep = 0;
        for (size_t i = 0; i < ret.size(); ++i) {
            if (GpuPassed(ret[i].stamp)) {
                if (ret[i].res) ret[i].res->Release();
            } else
                ret[keep++] = ret[i];
        }
        ret.resize(keep);
    }
    GpuBracket _gt(cl, GT_UploadFlush);
    for (PendingUpload& u : todo) {
        WrapResource* wr = AsWrapResource(u.dst);
        // RINGSTALE: did this upload outwait the ring's protection before its copy is recorded?
        if (u.from_ring && wr && wr->dx12 && GpuPassed(u.queued_stamp)) {
            const unsigned long long n = g_ring_expired.fetch_add(1) + 1;
            if (n <= 16 || (n % 256) == 0) {
                const D3D12_RESOURCE_DESC rd = wr->dx12->GetDesc();
                logger::write("RINGSTALE expired #%llu: %s dst=%p %llux%u fmt=%d mips=%u, %llu B: the GPU passed its "
                              "ring stamp %llu before its copy was recorded -- unprotected while it waited",
                    n, u.is_texture ? "texture" : "buffer", (void*)wr->dx12,
                    (unsigned long long)rd.Width, (unsigned)rd.Height, (int)rd.Format,
                    (unsigned)rd.MipLevels, (unsigned long long)u.bytes, (unsigned long long)u.queued_stamp);
            }
        }
        if (wr && wr->dx12) {
            Transition(cl, wr, D3D12_RESOURCE_STATE_COPY_DEST);
            if (u.is_texture) {
                for (UINT i = 0; i < u.subresources; ++i) {
                    D3D12_TEXTURE_COPY_LOCATION src{};
                    src.pResource = u.staging;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src.PlacedFootprint = u.layouts[i];
                    D3D12_TEXTURE_COPY_LOCATION dst{};
                    dst.pResource = wr->dx12;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = i;
                    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                ProfGpuWork(PGW_UploadTex, u.bytes);
                RemovalPin::Ours(cl, "port-upload-tex");
                TexTiming::NoteFlushed(wr->dx12, u.bytes);
            } else {
                cl->CopyBufferRegion(wr->dx12, 0, u.staging, u.src_offset, u.bytes);
                ProfGpuWork(PGW_Upload, u.bytes);
                RemovalPin::Ours(cl, "port-upload");
            }
            Transition(cl, wr, D3D12_RESOURCE_STATE_COMMON);
        }
        std::lock_guard<std::mutex> l(Mtx());
        if (u.from_ring) {
            const UINT64 lo = u.is_texture ? (u.layouts.empty() ? u.src_offset : u.layouts.front().Offset)
                                           : u.src_offset;
            RingTouch(lo, u.bytes);
        } else {
            RetiredStaging().push_back({ u.staging, GpuStamp() });
        }
        if (u.dst) u.dst->Release();
    }
    logger::trace("PORT: flushed %zu queued upload(s) onto the command list", todo.size());
}

constexpr UINT kRingBlock = 256;
struct RetiredHeap {
    ID3D12DescriptorHeap* heap;
    uint64_t              tag;
};
std::vector<RetiredHeap>& RetiredRings()
{
    static auto* v = new std::vector<RetiredHeap>();
    return *v;
}
bool RingReserve(Heap& ring, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n)
{
    const uint64_t now = GpuStamp();
    auto&          old = RetiredRings();
    for (size_t i = 0; i < old.size();)
        if (GpuPassed(old[i].tag)) {
            old[i].heap->Release();
            old[i] = old.back();
            old.pop_back();
        } else
            ++i;
    if (!n) return true;
    for (;;) {
        const UINT blocks = (ring.size + kRingBlock - 1) / kRingBlock;
        if (ring.tag.size() != blocks) ring.tag.assign(blocks, 0);
        UINT start = ring.next;
        if (start + n > ring.size) start = 0; // a run never straddles the end
        const UINT first = (start == ring.next && start % kRingBlock) ? start / kRingBlock + 1 : start / kRingBlock;
        const UINT last = (start + n - 1) / kRingBlock;
        bool       free = start + n <= ring.size;
        for (UINT b = first; free && b <= last; ++b)
            if (ring.tag[b] && !GpuPassed(ring.tag[b])) free = false;
        if (free) {
            for (UINT b = start / kRingBlock; b <= last; ++b) ring.tag[b] = now;
            ring.next = start;
            return true;
        }
        const Heap was = ring;
        UINT       size = ring.size * 2;
        while (size < n) size *= 2;
        if (!MakeHeap(ring, H().dev, type, size, true)) {
            ring = was;
            return false;
        }
        ring.tag.clear();
        old.push_back({ was.heap, now });
        logger::write("PORT: %s ring grown to %u descriptors (its slots were still in flight)",
            type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? "sampler" : "CBV/SRV/UAV", size);
    }
}

namespace {
thread_local bool t_heaps_changed = false;
} // namespace

bool TakeHeapsChanged()
{
    const bool c = t_heaps_changed;
    t_heaps_changed = false;
    return c;
}

bool ShaderVisibleCopy(D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu,
    D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, ID3D12GraphicsCommandList* cl)
{
    if (!EnsureHeaps() || !src.ptr) return false;
    std::lock_guard<std::mutex> l(Mtx());
    Heap&                       ring = H().ring;
    if (!RingReserve(ring, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1)) return false;
    const UINT slot = ring.next++;
    H().dev->CopyDescriptorsSimple(1, ring.Cpu(slot), src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (cl) {
        // ClearUnorderedAccessView* reads the GPU handle, so the heap holding it must be the one set.
        ID3D12DescriptorHeap* heaps[2] = { ring.heap, H().samp_ring.heap };
        cl->SetDescriptorHeaps(2, heaps);
        t_heaps_changed = true;
    }
    if (out_gpu) *out_gpu = ring.Gpu(slot);
    if (out_cpu) *out_cpu = src;
    return true;
}

bool EnsureHeaps()
{
    static std::atomic<ID3D12Device*> s_ready_dev{ nullptr };
    const Dx12Bridge&                 b = GetDx12Bridge();
    if (!b.device) return false;
    if (b.device == s_ready_dev.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> l(Mtx());
    if (H().dev == b.device && H().staging.heap) return true;
    if (H().dev != b.device) {
        // The device changed (or arrived). Everything cached belonged to the old one.
        H().dev = b.device;
    }
    if (H().staging.heap) return true;
    const bool ok =
        MakeHeap(H().staging, H().dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kStagingSize, false) &&
        MakeHeap(H().ring, H().dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRingSize, true) &&
        MakeHeap(H().rtv, H().dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSize, false) &&
        MakeHeap(H().dsv, H().dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvSize, false) &&
        MakeHeap(H().samp_staging, H().dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSampStaging, false) &&
        MakeHeap(H().samp_ring, H().dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSampRing, true);
    if (!ok) {
        logger::write("PORT: descriptor heap creation FAILED -- custom shaders cannot run");
        return false;
    }
    logger::write("PORT: descriptor heaps ready (staging %u, ring %u, rtv %u, dsv %u)",
        kStagingSize, kRingSize, kRtvSize, kDsvSize);
    s_ready_dev.store(b.device, std::memory_order_release);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE AllocStagingDescriptor(char kind, UINT* out_slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE none{};
    if (!EnsureHeaps()) return none;
    std::lock_guard<std::mutex> l(Mtx());
    Heap*                       h = StagingFor(kind);
    if (!h || !h->heap) return none;
    UINT slot;
    if (!h->freed.empty()) {
        slot = h->freed.back();
        h->freed.pop_back();
    } else {
        if (h->next == h->size * (UINT)(1 + h->more.size())) { // every block taken: one more
            D3D12_DESCRIPTOR_HEAP_DESC d = h->heap->GetDesc();
            ID3D12DescriptorHeap*      nb = nullptr;
            if (FAILED(H().dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&nb)))) return none;
            h->more.push_back(nb);
            logger::write("PORT: staging descriptor heap '%c' grown to %u slots (%zu live views)",
                kind, h->size * (UINT)(1 + h->more.size()), (size_t)(h->next - h->freed.size()));
        }
        slot = h->next++;
    }
    if (out_slot) *out_slot = slot;
    return h->Cpu(slot);
}

void ReleaseStagingDescriptor(char kind, UINT slot)
{
    std::lock_guard<std::mutex> l(Mtx());
    Heap*                       h = StagingFor(kind);
    if (h && h->heap && slot < h->next) h->freed.push_back(slot);
}

bool ReflectShader(ShaderCode& sc)
{
    if (sc.reflected) return true;
    sc.reflected = true; // once, pass or fail: a failure will not start passing
    if (sc.code.empty()) return false;

    ID3D12ShaderReflection* refl = nullptr;
    HRESULT                 hr = D3DReflect(sc.code.data(), sc.code.size(), IID_PPV_ARGS(&refl));
    if (FAILED(hr) || !refl) {
        logger::write("PORT: D3DReflect FAILED for %c shader (%zu bytes) hr=%08lX -- root signature "
                      "cannot be derived",
            sc.stage, sc.code.size(), (unsigned long)hr);
        return false;
    }
    D3D12_SHADER_DESC sd{};
    if (SUCCEEDED(refl->GetDesc(&sd))) {
        for (UINT i = 0; i < sd.BoundResources; ++i) {
            D3D12_SHADER_INPUT_BIND_DESC bd{};
            if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
            char kind = 0;
            switch (bd.Type) {
            case D3D_SIT_CBUFFER:
                kind = 'b';
                if (bd.BindPoint < 16) {
                    if (ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName(bd.Name)) {
                        D3D12_SHADER_BUFFER_DESC cbd{};
                        if (SUCCEEDED(cb->GetDesc(&cbd)) && cbd.Size)
                            sc.cb_size[bd.BindPoint] = cbd.Size;
                    }
                }
                break;
            case D3D_SIT_SAMPLER: kind = 's'; break;
            case D3D_SIT_UAV_RWTYPED:
            case D3D_SIT_UAV_RWSTRUCTURED:
            case D3D_SIT_UAV_RWBYTEADDRESS:
            case D3D_SIT_UAV_APPEND_STRUCTURED:
            case D3D_SIT_UAV_CONSUME_STRUCTURED:
            case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: kind = 'u'; break;
            case D3D_SIT_TBUFFER:
            case D3D_SIT_TEXTURE:
            case D3D_SIT_STRUCTURED:
            case D3D_SIT_BYTEADDRESS: kind = 't'; break;
            default: kind = 't'; break;
            }
            // An array binding occupies BindCount consecutive registers; each needs its own table entry.
            for (UINT k = 0; k < (bd.BindCount ? bd.BindCount : 1); ++k)
                sc.regs.push_back(ShaderReg{ kind, bd.BindPoint + k, bd.Space,
                    (unsigned char)bd.Type, (unsigned char)bd.Dimension });
        }
    }
    refl->Release();
    return true;
}

PipeState& State()
{
    static PipeState s;
    return s;
}

namespace {

// Gather the root bindings this draw needs: every register every bound shader actually uses.
bool CollectBinds(bool compute, std::vector<RootBind>& out, UINT& n_srv_uav_cbv, UINT& n_samp)
{
    PipeState& st = State();
    n_srv_uav_cbv = n_samp = 0;
    const char* stages = compute ? "c" : "vhdgp";
    for (const char* s = stages; *s; ++s) {
        const int   si = StageIndex(*s);
        ShaderCode* sc = AsShaderCode(st.shader[si]);
        if (!sc) continue;
        if (!ReflectShader(*sc)) return false;
        for (const ShaderReg& r : sc->regs) {
            out.push_back(RootBind{ (UINT)out.size(), *s, r.kind, r.reg, r.space, r.sit, r.dim });
            if (r.kind == 's')
                ++n_samp;
            else
                ++n_srv_uav_cbv;
        }
    }
    if (out.size() > 64) {
        // 64 DWORDs is the hard root-signature limit and each table costs one.
        logger::write("PORT: %zu descriptor tables exceeds the 64-DWORD root signature limit", out.size());
        return false;
    }
    return true;
}

bool g_bd_trace = false;

} // namespace
std::atomic<uint32_t> g_cs_uav_bound_mask{ 0 }; // bit N = compute uN was bound this dispatch

namespace {

static bool HasDescriptor(const WrapView* wv, char kind, UINT reg)
{
    if (wv->cpu.ptr) return true;
    static std::mutex                         m;
    static std::unordered_map<unsigned, bool> seen;
    bool                                      fresh;
    {
        std::lock_guard<std::mutex> l(m);
        fresh = seen.emplace(((unsigned)kind << 8) | reg, true).second;
    }
    if (fresh)
        logger::write("PORT: %c%u is bound to a view with NO D3D12 DESCRIPTOR (WrapView::cpu is null) -- "
                      "binding a null descriptor instead. Whoever built that view skipped the staging "
                      "allocation that Device::Create*View does; the shader will read zeros from it.",
            kind, reg);
    return false;
}

bool BindDescriptors(ID3D12GraphicsCommandList* cl, const RootSigEntry& rse, bool compute)
{
    PipeState& st = State();
    if (g_bd_trace) {
        std::string b;
        char        tmp[64];
        for (const RootBind& rb : rse.binds) {
            const int   si = StageIndex(rb.stage);
            const void* bound = nullptr;
            if (rb.kind == 'b')
                bound = (si >= 0 && rb.reg < kMaxCB) ? (void*)st.cb[si][rb.reg] : nullptr;
            else if (rb.kind == 'u')
                bound = (rb.reg < kMaxUAV) ? (void*)st.uav[rb.reg] : nullptr;
            else if (rb.kind == 't')
                bound = (si >= 0 && rb.reg < kMaxSRV) ? (void*)st.srv[si][rb.reg] : nullptr;
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " %c%c%u=%s", rb.stage, rb.kind, rb.reg,
                bound ? "bound" : "NULL");
            b += tmp;
        }
        logger::write("BINDDESC enter binds=%zu compute=%d --%s", rse.binds.size(), (int)compute, b.c_str());
    }
    std::lock_guard<std::mutex> l(Mtx());
    if (g_bd_trace) logger::write("BINDDESC locked -- Mtx() acquired");

    {
        UINT n_res = 0;
        UINT n_smp = 0;
        for (const RootBind& b : rse.binds) (b.kind == 's' ? n_smp : n_res)++;
        if (!RingReserve(H().ring, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, n_res) ||
            !RingReserve(H().samp_ring, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, n_smp)) return false;
    }
    ID3D12DescriptorHeap* heaps[2] = { H().ring.heap, H().samp_ring.heap };
    cl->SetDescriptorHeaps(2, heaps);
    t_heaps_changed = true;

    for (const RootBind& b : rse.binds) {
        const int                         si = StageIndex(b.stage);
        Heap&                             ring = (b.kind == 's') ? H().samp_ring : H().ring;
        const UINT                        slot = ring.next++; // reserved above: in range and out of flight
        const D3D12_CPU_DESCRIPTOR_HANDLE dst = ring.Cpu(slot);
        if (g_bd_trace)
            logger::write("BINDDESC   bind param=%u %c%c%u slot=%u/%u heap=%p cpu=%zx",
                b.param, b.stage, b.kind, b.reg, slot, ring.size,
                (void*)ring.heap, (size_t)dst.ptr);
        // Record compute UAV registers as they are bound, for the capture to report per pass.
        if (compute && b.kind == 'u' && b.reg < 32)
            g_cs_uav_bound_mask.fetch_or(1u << b.reg, std::memory_order_relaxed);

        if (b.kind == 'b') {
            ID3D11Buffer* cb = (si >= 0 && b.reg < kMaxCB) ? st.cb[si][b.reg] : nullptr;
            if (cb) {
                WrapResource* wr = AsWrapResource(cb);
                Transition(cl, wr, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                WriteCbvDescriptor(cb, dst);
            } else {
                WriteNullDescriptor('b', dst, b.sit, b.dim);
            }
        } else if (b.kind == 'u') {
            ID3D11UnorderedAccessView* v = (b.reg < kMaxUAV) ? st.uav[b.reg] : nullptr;
            WrapView*                  wv = AsWrapView(v);
            if (wv && wv->wres && HasDescriptor(wv, 'u', b.reg)) {
                Transition(cl, wv->wres, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                H().dev->CopyDescriptorsSimple(1, dst, wv->cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            } else {
                WriteNullDescriptor('u', dst, b.sit, b.dim);
            }
        } else if (b.kind == 's') {
            ID3D11SamplerState* ss = (si >= 0 && b.reg < kMaxSamp) ? st.samp[si][b.reg] : nullptr;
            StateDesc*          sd = AsStateDesc(ss);
            D3D12_SAMPLER_DESC  d{};
            if (sd) {
                d.Filter = (D3D12_FILTER)sd->sampler.Filter;
                d.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)sd->sampler.AddressU;
                d.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)sd->sampler.AddressV;
                d.AddressW = (D3D12_TEXTURE_ADDRESS_MODE)sd->sampler.AddressW;
                d.MipLODBias = sd->sampler.MipLODBias;
                d.MaxAnisotropy = sd->sampler.MaxAnisotropy;
                d.ComparisonFunc = (D3D12_COMPARISON_FUNC)sd->sampler.ComparisonFunc;
                for (int i = 0; i < 4; ++i) d.BorderColor[i] = sd->sampler.BorderColor[i];
                d.MinLOD = sd->sampler.MinLOD;
                d.MaxLOD = sd->sampler.MaxLOD;
            } else {
                // The DX11 default sampler, which is what an unbound slot reads as.
                d.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                d.AddressU = d.AddressV = d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                d.MaxLOD = D3D12_FLOAT32_MAX;
                d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            }
            H().dev->CreateSampler(&d, dst);
        } else { // 't'
            ID3D11ShaderResourceView* v = (si >= 0 && b.reg < kMaxSRV) ? st.srv[si][b.reg] : nullptr;
            WrapView*                 wv = AsWrapView(v);
            if (wv && wv->wres && HasDescriptor(wv, 't', b.reg)) {
                Transition(cl, wv->wres, compute ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                H().dev->CopyDescriptorsSimple(1, dst, wv->cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            } else {
                WriteNullDescriptor('t', dst, b.sit, b.dim);
            }
        }

        const D3D12_GPU_DESCRIPTOR_HANDLE gpu = ring.Gpu(slot);
        if (compute)
            cl->SetComputeRootDescriptorTable(b.param, gpu);
        else
            cl->SetGraphicsRootDescriptorTable(b.param, gpu);
    }
    if (g_bd_trace) logger::write("BINDDESC done -- all descriptors written, returning to IssueDispatch");
    return true;
}

void TraceDispatch(const RootSigEntry& rse, UINT x, UINT y, UINT z)
{
    PipeState& st = State();
    wchar_t    line[512];

    ShaderCode* cs = AsShaderCode(st.shader[5]);
    _snwprintf_s(line, _TRUNCATE,
        L"  LEAF dispatch groups=%u,%u,%u cs=%zu bytes binds=%zu",
        x, y, z, cs ? cs->code.size() : (size_t)0, rse.binds.size());
    ObserveCommand(line);

    for (const RootBind& b : rse.binds) {
        const int si = StageIndex(b.stage);
        if (b.kind == 'b') {
            ID3D11Buffer*             cb = (si >= 0 && b.reg < kMaxCB) ? st.cb[si][b.reg] : nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS va = 0;
            UINT64                    size = 0;
            bool                      windowed = false;
            if (cb && CbvWindow(cb, &va, &size, &windowed)) {
                _snwprintf_s(line, _TRUNCATE,
                    L"  LEAF   %cb%u = res %p va %016llX size %llu %s",
                    b.stage, b.reg, (void*)cb, (unsigned long long)va,
                    (unsigned long long)size,
                    windowed ? L"(window into game ring)" : L"(whole buffer)");
            } else {
                _snwprintf_s(line, _TRUNCATE,
                    L"  LEAF   %cb%u = NULL DESCRIPTOR (reads as zero) cb=%p",
                    b.stage, b.reg, (void*)cb);
            }
            ObserveCommand(line);
        } else if (b.kind == 'u') {
            ID3D11UnorderedAccessView* v = (b.reg < kMaxUAV) ? st.uav[b.reg] : nullptr;
            WrapView*                  wv = AsWrapView(v);
            if (wv && wv->wres) {
                // The D3D11 desc lives on the concrete WrapViewT, and `kind` is what says which one it is.
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                if (wv->kind == 'u') ud = static_cast<const WrapUAV&>(*wv).desc;
                _snwprintf_s(line, _TRUNCATE,
                    L"  LEAF   %cu%u = view %p res %p fmt %u first %u elems %u state %u",
                    b.stage, b.reg, (void*)v, (void*)wv->wres->dx12,
                    (unsigned)ud.Format,
                    (unsigned)ud.Buffer.FirstElement,
                    (unsigned)ud.Buffer.NumElements,
                    (unsigned)wv->wres->state);
            } else {
                _snwprintf_s(line, _TRUNCATE,
                    L"  LEAF   %cu%u = NULL DESCRIPTOR (writes discarded) uav=%p",
                    b.stage, b.reg, (void*)v);
            }
            ObserveCommand(line);
        } else if (b.kind == 't') {
            ID3D11ShaderResourceView* v = (si >= 0 && b.reg < kMaxSRV) ? st.srv[si][b.reg] : nullptr;
            WrapView*                 wv = AsWrapView(v);
            _snwprintf_s(line, _TRUNCATE,
                L"  LEAF   %ct%u = %s view %p res %p",
                b.stage, b.reg, (wv && wv->wres) ? L"srv" : L"NULL DESCRIPTOR (reads as zero)",
                (void*)v, (void*)((wv && wv->wres) ? wv->wres->dx12 : nullptr));
            ObserveCommand(line);
        }
    }
}

static uint64_t ContentHash(ShaderCode* s)
{
    if (!s) return 0;
    if (!s->content_hash) s->content_hash = Fnv1a64(s->code.data(), s->code.size()) | 1ull;
    return s->content_hash;
}
static uint64_t ContentHash(StateDesc* s)
{
    if (!s) return 0;
    if (!s->content_hash) {
        uint64_t h = Fnv1a64(&s->kind, sizeof(s->kind));
        switch (s->kind) {
        case 'B': h = Fnv1a64(&s->blend, sizeof(s->blend), h); break;
        case 'R': h = Fnv1a64(&s->raster, sizeof(s->raster), h); break;
        case 'D': h = Fnv1a64(&s->depth, sizeof(s->depth), h); break;
        default: h = Fnv1a64(&s->sampler, sizeof(s->sampler), h); break;
        }
        s->content_hash = h | 1ull;
    }
    return s->content_hash;
}
static uint64_t ContentHash(InputLayoutDesc* l)
{
    if (!l) return 0;
    if (!l->content_hash) {
        uint64_t h = Fnv1a64("il", 2);
        for (const D3D12_INPUT_ELEMENT_DESC& e : l->elems) {
            if (e.SemanticName) h = Fnv1a64(e.SemanticName, strlen(e.SemanticName) + 1, h);
            h = Fnv1a64(&e.SemanticIndex, sizeof(e.SemanticIndex), h);
            h = Fnv1a64(&e.Format, sizeof(e.Format), h);
            h = Fnv1a64(&e.InputSlot, sizeof(e.InputSlot), h);
            h = Fnv1a64(&e.AlignedByteOffset, sizeof(e.AlignedByteOffset), h);
            h = Fnv1a64(&e.InputSlotClass, sizeof(e.InputSlotClass), h);
            h = Fnv1a64(&e.InstanceDataStepRate, sizeof(e.InstanceDataStepRate), h);
        }
        l->content_hash = h | 1ull;
    }
    return l->content_hash;
}

ID3D12PipelineState* GetGraphicsPso(ID3D12RootSignature* rs, DXGI_FORMAT rt_fmt[8], UINT num_rt,
    DXGI_FORMAT ds_fmt)
{
    PipeState&  st = State();
    ShaderCode* vs = AsShaderCode(st.shader[0]);
    ShaderCode* hs = AsShaderCode(st.shader[1]);
    ShaderCode* ds = AsShaderCode(st.shader[2]);
    ShaderCode* gs = AsShaderCode(st.shader[3]);
    ShaderCode* ps = AsShaderCode(st.shader[4]);
    if (!vs || vs->code.empty()) return nullptr; // handled by IssueDraw's state-only path
    StateDesc*       bl = AsStateDesc(st.blend);
    StateDesc*       ra = AsStateDesc(st.raster);
    StateDesc*       de = AsStateDesc(st.depth);
    InputLayoutDesc* il = AsInputLayout(st.layout);

    uint64_t key = Fnv1a64(&rs, sizeof(rs));
    for (ShaderCode* s : { vs, hs, ds, gs, ps }) {
        const uint64_t h = ContentHash(s);
        key = Fnv1a64(&h, sizeof(h), key);
    }
    key = Fnv1a64(rt_fmt, sizeof(DXGI_FORMAT) * 8, key);
    key = Fnv1a64(&num_rt, sizeof(num_rt), key);
    key = Fnv1a64(&ds_fmt, sizeof(ds_fmt), key);
    key = Fnv1a64(&st.topology, sizeof(st.topology), key);
    key = Fnv1a64(&st.sample_mask, sizeof(st.sample_mask), key);
    {
        uint64_t h;
        h = ContentHash(bl);
        key = Fnv1a64(&h, sizeof(h), key);
        h = ContentHash(ra);
        key = Fnv1a64(&h, sizeof(h), key);
        h = ContentHash(de);
        key = Fnv1a64(&h, sizeof(h), key);
        h = ContentHash(il);
        key = Fnv1a64(&h, sizeof(h), key);
    }

    auto it = H().psos.find(key);
    if (it != H().psos.end()) return it->second;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.VS = { vs->code.data(), vs->code.size() };
    if (hs && !hs->code.empty()) pd.HS = { hs->code.data(), hs->code.size() };
    if (ds && !ds->code.empty()) pd.DS = { ds->code.data(), ds->code.size() };
    if (gs && !gs->code.empty()) pd.GS = { gs->code.data(), gs->code.size() };
    if (ps && !ps->code.empty()) pd.PS = { ps->code.data(), ps->code.size() };
    FillBlend(pd.BlendState, bl ? &bl->blend : nullptr);
    FillRaster(pd.RasterizerState, ra ? &ra->raster : nullptr);
    FillDepth(pd.DepthStencilState, de ? &de->depth : nullptr, ds_fmt != DXGI_FORMAT_UNKNOWN);
    pd.SampleMask = st.sample_mask;
    pd.PrimitiveTopologyType = TopoType(st.topology);
    pd.NumRenderTargets = num_rt;
    for (UINT i = 0; i < 8; ++i) pd.RTVFormats[i] = (i < num_rt) ? rt_fmt[i] : DXGI_FORMAT_UNKNOWN;
    pd.DSVFormat = ds_fmt;
    pd.SampleDesc = { 1, 0 };
    if (il && !il->elems.empty()) pd.InputLayout = { il->elems.data(), (UINT)il->elems.size() };

    ID3D12PipelineState* pso = nullptr;
    HRESULT              hr = H().dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        logger::write("PORT: graphics PSO FAILED hr=%08lX (vs=%zu ps=%zu gs=%zu rt=%u fmt=%d ds=%d)",
            (unsigned long)hr, vs->code.size(), ps ? ps->code.size() : 0,
            gs ? gs->code.size() : 0, num_rt, (int)rt_fmt[0], (int)ds_fmt);
        H().psos[key] = nullptr; // remember the failure; retrying every draw would spam and stall
        return nullptr;
    }
    H().psos[key] = pso;
    logger::write("PORT: graphics PSO built (vs%s%s%s, %u RT fmt=%d ds=%d)",
        ps ? "+ps" : "", gs ? "+gs" : "", (hs || ds) ? "+tess" : "",
        num_rt, (int)rt_fmt[0], (int)ds_fmt);
    return pso;
}

ID3D12PipelineState* GetComputePso(ID3D12RootSignature* rs)
{
    PipeState&  st = State();
    ShaderCode* cs = AsShaderCode(st.shader[5]);
    if (!cs || cs->code.empty()) return nullptr; // reported by IssueDispatch, which knows it is a no-op
    uint64_t key = Fnv1a64(&rs, sizeof(rs));
    {
        const uint64_t h = ContentHash(cs);
        key = Fnv1a64(&h, sizeof(h), key);
    }
    key = Fnv1a64("cs", 2, key);

    auto it = H().psos.find(key);
    if (it != H().psos.end()) return it->second;

    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = rs;
    cd.CS = { cs->code.data(), cs->code.size() };
    ID3D12PipelineState* pso = nullptr;
    HRESULT              hr = H().dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        logger::write("PORT: compute PSO FAILED hr=%08lX (%zu bytes)", (unsigned long)hr, cs->code.size());
        H().psos[key] = nullptr;
        return nullptr;
    }
    H().psos[key] = pso;
    logger::write("PORT: compute PSO built (%zu bytes, %zu bound registers)", cs->code.size(),
        cs->regs.size());
    return pso;
}

} // namespace

static std::atomic<int>                g_present_draw_probe{ 0 };
static std::atomic<unsigned long long> g_own_list_draws{ 0 };
unsigned long long                     OwnListDrawsIssued()
{
    return g_own_list_draws.load(std::memory_order_relaxed);
}
void ArmPresentDrawProbe(int frames)
{
    g_present_draw_probe.store(frames);
    logger::write("PRESENTDRAW armed for %d [Present] frame(s)", frames);
}
bool PresentDrawProbeArmed()
{
    return g_present_draw_probe.load(std::memory_order_relaxed) > 0;
}
void PresentDrawProbeFrameDone()
{
    int v = g_present_draw_probe.load();
    if (v > 0 && g_present_draw_probe.compare_exchange_strong(v, v - 1) && v == 1)
        logger::write("PRESENTDRAW complete");
}

static bool IssueBail(bool draw, const char* gate, const void* shader, UINT a, UINT b, UINT c)
{
    if (!logger::enabled(1)) return false;
    constexpr size_t   kRecent = 16;
    static std::mutex  m;
    static std::string recent[2][kRecent];
    static size_t      next[2] = {};
    char               sig[192];
    _snprintf_s(sig, sizeof(sig), _TRUNCATE, "%s|%p|%u,%u,%u", gate, shader, a, b, c);
    bool changed = true;
    {
        std::lock_guard<std::mutex> l(m);
        std::string*                ring = recent[draw ? 1 : 0];
        for (size_t i = 0; i < kRecent && changed; ++i)
            if (ring[i] == sig) changed = false;
        if (changed) {
            ring[next[draw ? 1 : 0]] = sig;
            next[draw ? 1 : 0] = (next[draw ? 1 : 0] + 1) % kRecent;
        }
    }
    if (draw && (changed || PresentDrawProbeArmed()))
        logger::write("DRAW-BAIL %-18s vs=%p count=%u start=%u inst=%u own_list=%d -- NO Draw issued",
            gate, shader, a, b, c, (int)GetDx12Bridge().own_list);
    else if (!draw && changed)
        logger::write("DISPATCH-BAIL %-14s cs=%p groups=%u,%u,%u -- NO Dispatch issued", gate, shader, a, b, c);
    return false;
}
static bool DispatchBail(const char* gate, const void* cs, UINT x, UINT y, UINT z)
{
    return IssueBail(false, gate, cs, x, y, z);
}

static bool UploadPendingFor(ID3D11Resource* res)
{
    std::lock_guard<std::mutex> l(Mtx());
    for (const PendingUpload& u : Uploads())
        if (u.dst == res) return true;
    return false;
}

static void TracePresentDraw(const Dx12Bridge& b, const RootSigEntry& rse, bool indexed, UINT count,
    UINT start, UINT instances, UINT num_rt, DXGI_FORMAT rt0_fmt,
    ID3D12Resource* rt0, unsigned rt0_tracked, UINT rtv_declared,
    DXGI_FORMAT ds_fmt, int vp_mode, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc)
{
    PipeState& st = State();
    logger::write("PRESENTDRAW draw tid=%lu cl=%p own_list=%d bb=%p | vs=%p hs=%p ds=%p gs=%p ps=%p "
                  "topo=%u %s count=%u start=%u inst=%u",
        GetCurrentThreadId(), (void*)b.cl, (int)b.own_list, (void*)b.back_buffer,
        (void*)st.shader[0], (void*)st.shader[1], (void*)st.shader[2], (void*)st.shader[3],
        (void*)st.shader[4], (unsigned)st.topology, indexed ? "indexed" : "plain",
        count, start, instances);
    logger::write("PRESENTDRAW   rt=%u (declared %u) rt0=%p is_bb=%d fmt=%d tracked=0x%x observed=0x%x "
                  "ds_fmt=%d | vp %s %.1f,%.1f %.1fx%.1f depth %.2f..%.2f | scissor %ld,%ld..%ld,%ld",
        num_rt, rtv_declared, (void*)rt0, (int)(rt0 && rt0 == b.back_buffer), (int)rt0_fmt,
        rt0_tracked, rt0 ? QueryResourceState(rt0) : 0u, (int)ds_fmt,
        vp_mode == 1 ? "SET" : (vp_mode == 2 ? "INHERITED+scissor" : "INHERITED"),
        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth,
        (long)sc.left, (long)sc.top, (long)sc.right, (long)sc.bottom);
    const StateDesc*                      bl = AsStateDesc(st.blend);
    const StateDesc*                      ra = AsStateDesc(st.raster);
    const StateDesc*                      de = AsStateDesc(st.depth);
    const D3D11_RENDER_TARGET_BLEND_DESC* rtb = bl ? &bl->blend.RenderTarget[0] : nullptr;
    logger::write("PRESENTDRAW   blend %s en=%d src=%d dst=%d op=%d srcA=%d dstA=%d mask=0x%x a2c=%d "
                  "factor=%.2f,%.2f,%.2f,%.2f | raster %s fill=%d cull=%d ccw=%d clip=%d scissor=%d | "
                  "depth %s en=%d func=%d | sample_mask=0x%x",
        bl ? "SET" : "default", rtb ? (int)rtb->BlendEnable : -1, rtb ? (int)rtb->SrcBlend : -1,
        rtb ? (int)rtb->DestBlend : -1, rtb ? (int)rtb->BlendOp : -1,
        rtb ? (int)rtb->SrcBlendAlpha : -1, rtb ? (int)rtb->DestBlendAlpha : -1,
        rtb ? (unsigned)rtb->RenderTargetWriteMask : 0xFFu, bl ? (int)bl->blend.AlphaToCoverageEnable : -1,
        st.blend_factor[0], st.blend_factor[1], st.blend_factor[2], st.blend_factor[3],
        ra ? "SET" : "default", ra ? (int)ra->raster.FillMode : -1, ra ? (int)ra->raster.CullMode : -1,
        ra ? (int)ra->raster.FrontCounterClockwise : -1, ra ? (int)ra->raster.DepthClipEnable : -1,
        ra ? (int)ra->raster.ScissorEnable : -1,
        de ? "SET" : "default", de ? (int)de->depth.DepthEnable : -1, de ? (int)de->depth.DepthFunc : -1,
        (unsigned)st.sample_mask);
    std::string binds;
    char        tmp[256];
    for (const RootBind& rb : rse.binds) {
        const int si = StageIndex(rb.stage);
        tmp[0] = 0;
        if (rb.kind == 't') {
            WrapView* wv = AsWrapView((si >= 0 && rb.reg < kMaxSRV) ? st.srv[si][rb.reg] : nullptr);
            if (!wv || !wv->wres || !wv->wres->dx12) {
                _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " %ct%u=NULL", rb.stage, rb.reg);
            } else {
                const D3D12_RESOURCE_DESC d = wv->wres->dx12->GetDesc();
                _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " %ct%u=%p(%llux%u f%d m%u st=0x%x%s%s)", rb.stage,
                    rb.reg, (void*)wv->wres->dx12, (unsigned long long)d.Width, (unsigned)d.Height,
                    (int)d.Format, (unsigned)d.MipLevels, (unsigned)wv->wres->state,
                    UploadPendingFor(wv->res) ? " PENDING" : "", wv->cpu.ptr ? "" : " NO-DESCRIPTOR");
            }
        } else if (rb.kind == 'b') {
            ID3D11Buffer* cb = (si >= 0 && rb.reg < kMaxCB) ? st.cb[si][rb.reg] : nullptr;
            WrapResource* wr = AsWrapResource(cb);
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " %cb%u=%p(%lluB)", rb.stage, rb.reg,
                wr ? (void*)wr->dx12 : nullptr,
                (wr && wr->dx12) ? (unsigned long long)wr->dx12->GetDesc().Width : 0ull);
        } else if (rb.kind == 's') {
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " %cs%u=%s", rb.stage, rb.reg,
                (si >= 0 && rb.reg < kMaxSamp && AsStateDesc(st.samp[si][rb.reg])) ? "set" : "default");
        } else {
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " %cu%u=%p", rb.stage, rb.reg,
                (rb.reg < kMaxUAV) ? (void*)st.uav[rb.reg] : nullptr);
        }
        binds += tmp;
    }
    logger::write("PRESENTDRAW   binds:%s", binds.c_str());
    for (const RootBind& rb : rse.binds) {
        if (rb.kind != 't') continue;
        const int si = StageIndex(rb.stage);
        WrapView* wv = AsWrapView((si >= 0 && rb.reg < kMaxSRV) ? st.srv[si][rb.reg] : nullptr);
        if (!wv || !wv->wres || wv->wres->shadow.size() < 16 || wv->wres->shadow.size() > 65536) continue;
        const size_t n = wv->wres->shadow.size() / 16;
        const float* f = reinterpret_cast<const float*>(wv->wres->shadow.data());
        std::string  vals;
        unsigned     shown = 0;
        for (size_t i = 0; i < n && shown < 48; ++i) {
            const float* p = f + i * 4;
            if (!p[0] && !p[1] && !p[2] && !p[3]) continue;
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " [%zu]=%g,%g,%g,%g", i, p[0], p[1], p[2], p[3]);
            vals += tmp;
            ++shown;
        }
        logger::write("PRESENTDRAW   %ct%u shadow: %zu float4, uploaded-matches-shadow=%d, non-zero:%s",
            rb.stage, rb.reg, n,
            (int)(wv->wres->uploaded.size() == wv->wres->shadow.size() &&
                memcmp(wv->wres->uploaded.data(), wv->wres->shadow.data(), wv->wres->shadow.size()) == 0),
            vals.empty() ? " (all zero)" : vals.c_str());
    }
}

bool IssueDraw(bool indexed, UINT count, UINT start, INT base_vertex,
    UINT instances, UINT start_instance)
{
    PROF(PL_IssueDraw);
    Dx12Bridge  b = GetDx12Bridge(); // by value -- the same window as IssueDispatch below
    const void* vs_dbg = (const void*)State().shader[0];
    if (!b.cl || !b.device) {
        NotImplOnce("IssueDraw (no DX12 command list bound)");
        return IssueBail(true, !b.cl ? (b.device ? "no-cmdlist" : "no-cl+no-dev") : "no-device",
            vs_dbg, count, start, instances);
    }
    BridgeNoteIssue(b.cl, false, count, start, instances); // X30: is this the list the opening thread named?
    PayGameState();
    RemovalPin::Ours(b.cl, "port-draw");
    if (!EnsureHeaps()) return IssueBail(true, "EnsureHeaps", vs_dbg, count, start, instances);
    FlushPendingUploads(b.cl);
    PipeState& st = State();

    {
        static bool once = false;
        if (!once) {
            once = true;
            logger::write("ISSUEDRAW branch: vs=%d blend=%d depth=%d raster=%d topology=%u count=%u "
                          "(vs!=0 takes the FULL-PSO path; state-only needs vs==0)",
                (int)(AsShaderCode(st.shader[0]) != nullptr),
                (int)(AsStateDesc(st.blend) != nullptr), (int)(AsStateDesc(st.depth) != nullptr),
                (int)(AsStateDesc(st.raster) != nullptr), (unsigned)st.topology, count);
        }
    }

    if (!AsShaderCode(st.shader[0])) {
        StateDesc* bl = AsStateDesc(st.blend);
        StateDesc* de = AsStateDesc(st.depth);
        StateDesc* ra = AsStateDesc(st.raster);
        if (!bl && !de && !ra) {
            return IssueBail(true, "no-vs-no-state", vs_dbg, count, start, instances);
        }
        NotImplOnce("IssueDraw for a state-only [CustomShader] (blend/depth/raster with no shaders): "
                    "varying the GAME's PSO from here is not implemented (docs\\PORT-TRANSITIONS-REMAINING.md)");
        return IssueBail(true, "state-only-not-implemented", vs_dbg, count, start, instances);
    }

    DXGI_FORMAT                 rt_fmt[8] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_h[8] = {};
    UINT                        num_rt = 0;
    ID3D12Resource*             probe_rt0 = nullptr; // PRESENTDRAW: the first real target, and its state BEFORE ours
    unsigned                    probe_rt0_state = 0;
    for (UINT i = 0; i < st.num_rtv && i < kMaxRTV; ++i) {
        WrapView* wv = AsWrapView(st.rtv[i]);
        if (!wv || !wv->wres || !wv->wres->dx12 || !wv->cpu.ptr) continue; // a published game target has no descriptor: inherited
        if (!num_rt) {
            probe_rt0 = wv->wres->dx12;
            probe_rt0_state = (unsigned)wv->wres->state;
        }
        Transition(b.cl, wv->wres, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv_h[num_rt] = wv->cpu;
        rt_fmt[num_rt] = wv->wres->dx12->GetDesc().Format;
        ++num_rt;
    }
    WrapView* dsv = AsWrapView(st.dsv);
    if (dsv && !(dsv->wres && dsv->wres->dx12 && dsv->cpu.ptr)) dsv = nullptr;
    DXGI_FORMAT ds_fmt = DXGI_FORMAT_UNKNOWN;
    if (dsv) {
        Transition(b.cl, dsv->wres, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds_fmt = dsv->wres->dx12->GetDesc().Format;
    }
    const UINT bound_rt = num_rt;
    if (!num_rt) {
        if (b.rt_format == DXGI_FORMAT_UNKNOWN) {
            NotImplOnce("IssueDraw with no render target: the addon must publish Dx12Bridge::rt_format");
            return IssueBail(true, "no-rt-no-rt_format", vs_dbg, count, start, instances);
        }
        rt_fmt[0] = b.rt_format;
        num_rt = 1;
        if (ds_fmt == DXGI_FORMAT_UNKNOWN) ds_fmt = b.ds_format;
    }

    if (st.topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
        NotImplOnce("IssueDraw with no topology set: on DX11 the context keeps the game's, but a DX12 PSO "
                    "must state one -- the section needs `topology = ...`");
        return IssueBail(true, "no-topology", vs_dbg, count, start, instances);
    }

    std::vector<RootBind> want;
    UINT                  n_res = 0;
    UINT                  n_samp = 0;
    if (!CollectBinds(false, want, n_res, n_samp)) return IssueBail(true, "CollectBinds", vs_dbg, count, start, instances);
    RootSigEntry* rse = GetRootSig(want, false);
    if (!rse) return IssueBail(true, "GetRootSig", vs_dbg, count, start, instances);
    ID3D12PipelineState* pso = GetGraphicsPso(rse->rs, rt_fmt, num_rt, ds_fmt);
    if (!pso) return IssueBail(true, "GetGraphicsPso", vs_dbg, count, start, instances);

    b.cl->SetPipelineState(pso);
    b.cl->SetGraphicsRootSignature(rse->rs);
    if (!BindDescriptors(b.cl, *rse, false)) return IssueBail(true, "BindDescriptors", vs_dbg, count, start, instances);

    if (bound_rt || dsv)
        b.cl->OMSetRenderTargets(bound_rt, bound_rt ? rtv_h : nullptr, FALSE, dsv ? &dsv->cpu : nullptr);

    int            probe_vp_mode = 0; // PRESENTDRAW: 0 inherited, 1 set, 2 inherited + scissor set
    D3D12_VIEWPORT probe_vp{};
    D3D12_RECT     probe_sc{};
    if (st.has_viewport || b.own_list || !g_inherit_game_viewport) {
        D3D12_VIEWPORT vp = st.has_viewport
            ? D3D12_VIEWPORT{ st.viewport.TopLeftX, st.viewport.TopLeftY, st.viewport.Width,
                  st.viewport.Height, st.viewport.MinDepth, st.viewport.MaxDepth }
            : D3D12_VIEWPORT{ 0.0f, 0.0f, (float)b.rt_width, (float)b.rt_height, 0.0f, 1.0f };
        probe_vp = vp;
        if (vp.Width > 0.0f && vp.Height > 0.0f) {
            b.cl->RSSetViewports(1, &vp);
            D3D12_RECT sc = st.has_scissor
                ? D3D12_RECT{ st.scissor.left, st.scissor.top, st.scissor.right, st.scissor.bottom }
                : D3D12_RECT{ (LONG)vp.TopLeftX, (LONG)vp.TopLeftY,
                      (LONG)(vp.TopLeftX + vp.Width), (LONG)(vp.TopLeftY + vp.Height) };
            b.cl->RSSetScissorRects(1, &sc);
            g_touched_viewport = true; // and ONLY now does the restore have anything to put back
            probe_vp_mode = 1;
            probe_sc = sc;
        }
    } else if (st.has_scissor) {
        const D3D12_RECT sc{ st.scissor.left, st.scissor.top, st.scissor.right, st.scissor.bottom };
        b.cl->RSSetScissorRects(1, &sc);
        g_touched_viewport = true;
        probe_vp_mode = 2;
        probe_sc = sc;
    }

    b.cl->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)st.topology);
    if (AsStateDesc(st.blend)) {
        b.cl->OMSetBlendFactor(st.blend_factor);
        g_touched_blend = true;
    }
    if (AsStateDesc(st.depth)) {
        b.cl->OMSetStencilRef(st.stencil_ref);
        g_touched_stencil = true;
    }

    if (st.num_vb) {
        D3D12_VERTEX_BUFFER_VIEW vbv[kMaxVB] = {};
        UINT                     n = 0;
        for (UINT i = 0; i < st.num_vb && i < kMaxVB; ++i) {
            WrapResource* wr = AsWrapResource(st.vb[i]);
            if (!wr || !wr->dx12) continue;
            Transition(b.cl, wr, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            vbv[n].BufferLocation = wr->dx12->GetGPUVirtualAddress() + st.vb_offset[i];
            vbv[n].SizeInBytes = (UINT)(wr->dx12->GetDesc().Width - st.vb_offset[i]);
            vbv[n].StrideInBytes = st.vb_stride[i];
            ++n;
        }
        if (n) b.cl->IASetVertexBuffers(0, n, vbv);
    }
    if (indexed) {
        WrapResource* wr = AsWrapResource(st.ib);
        if (wr && wr->dx12) {
            Transition(b.cl, wr, D3D12_RESOURCE_STATE_INDEX_BUFFER);
            D3D12_INDEX_BUFFER_VIEW ibv{};
            ibv.BufferLocation = wr->dx12->GetGPUVirtualAddress() + st.ib_offset;
            ibv.SizeInBytes = (UINT)(wr->dx12->GetDesc().Width - st.ib_offset);
            ibv.Format = (st.ib_format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_R32_UINT
                                                               : st.ib_format;
            b.cl->IASetIndexBuffer(&ibv);
        }
    }

    if (PresentDrawProbeArmed() && logger::enabled(1))
        TracePresentDraw(b, *rse, indexed, count, start, instances ? instances : 1, num_rt, rt_fmt[0],
            probe_rt0, probe_rt0_state, st.num_rtv, ds_fmt, probe_vp_mode, probe_vp, probe_sc);

    g_owed_uav.Flush(b.cl);
    const int   gt = g_gpu_timer.Begin(b.cl, b.own_list ? GT_PresentDraw : GT_IssueDraw);
    void* const hp = HotPath::GpuBracketBegin(b.cl, b.own_list ? GT_PresentDraw : GT_IssueDraw);
    if (indexed)
        b.cl->DrawIndexedInstanced(count, instances ? instances : 1, start, base_vertex,
            start_instance);
    else
        b.cl->DrawInstanced(count, instances ? instances : 1, start, start_instance);
    HotPath::GpuBracketEnd(b.cl, hp);
    g_gpu_timer.End(b.cl, gt);
    if (b.own_list) g_own_list_draws.fetch_add(1, std::memory_order_relaxed);

    if (b.restore_game_state) b.restore_game_state(b.restore_user);
    return true;
}

static std::atomic<unsigned long long> g_dispatches_issued{ 0 };

static std::atomic<unsigned long long> g_dispatch_calls{ 0 };

bool IssueDispatch(UINT x, UINT y, UINT z)
{
    ++g_dispatch_calls;
    PROF(PL_IssueDispatch);
    Dx12Bridge  b = GetDx12Bridge();
    const void* cs_dbg = (const void*)State().shader[5];
    if (!b.cl || !b.device) {
        SkCapRecordBail(cs_dbg, x, y, z, b.device != nullptr);
        NotImplOnce("IssueDispatch (no DX12 command list bound)");
        return DispatchBail(!b.cl ? (b.device ? "no-cmdlist" : "no-cl+no-dev") : "no-device",
            cs_dbg, x, y, z);
    }
    BridgeNoteIssue(b.cl, true, x, y, z); // X30: is this the list the opening thread named?
    RemovalPin::Ours(b.cl, "port-dispatch");
    if (!EnsureHeaps()) return DispatchBail("EnsureHeaps", cs_dbg, x, y, z);
    FlushPendingUploads(b.cl);

    if (!AsShaderCode(State().shader[5])) {
        NotImplOnce("Dispatch with no compute shader bound (a no-op on DX11 as well)");
        return DispatchBail("no-cs-bound", cs_dbg, x, y, z);
    }

    std::vector<RootBind> want;
    UINT                  n_res = 0;
    UINT                  n_samp = 0;
    if (!CollectBinds(true, want, n_res, n_samp)) return DispatchBail("CollectBinds", cs_dbg, x, y, z);
    RootSigEntry* rse = GetRootSig(want, true);
    if (!rse) return DispatchBail("GetRootSig", cs_dbg, x, y, z);
    ID3D12PipelineState* pso = GetComputePso(rse->rs);
    if (!pso) return DispatchBail("GetComputePso", cs_dbg, x, y, z);

    {
        static std::mutex                           m;
        static std::unordered_map<const void*, int> seen;
        int                                         n;
        {
            std::lock_guard<std::mutex> l(m);
            n = ++seen[cs_dbg];
        }
        g_bd_trace = (n <= 2);
    }
    if (g_bd_trace) logger::write("DISPATCH-STEP 1/3 SetPipelineState cs=%p pso=%p", cs_dbg, (void*)pso);
    b.cl->SetPipelineState(pso);
    if (g_bd_trace) logger::write("DISPATCH-STEP 2/3 SetComputeRootSignature rs=%p", (void*)rse->rs);
    b.cl->SetComputeRootSignature(rse->rs);
    if (g_bd_trace) logger::write("DISPATCH-STEP 3/3 BindDescriptors");
    const bool bd_ok = BindDescriptors(b.cl, *rse, true);
    g_bd_trace = false;
    if (!bd_ok) return DispatchBail("BindDescriptors", cs_dbg, x, y, z);
    if (ObserverActive()) TraceDispatch(*rse, x, y, z);

    {
        static std::mutex                           m;
        static std::unordered_map<const void*, int> seen;
        const void*                                 key = (const void*)State().shader[5];
        int                                         n;
        {
            std::lock_guard<std::mutex> l(m);
            n = ++seen[key];
        }
        SkCapBeforeDispatch(b.device, b.cl, x, y, z);
        const int   gt = g_gpu_timer.Begin(b.cl, b.own_list ? GT_PresentDispatch : GT_IssueDispatch);
        void* const hp = HotPath::GpuBracketBegin(b.cl, b.own_list ? GT_PresentDispatch : GT_IssueDispatch);
        if (n <= 3) {
            logger::write("DISPATCH-PRE  cs=%p groups=%u,%u,%u (run %d)", key, x, y, z, n);
            b.cl->Dispatch(x, y, z);
            logger::write("DISPATCH-POST cs=%p groups=%u,%u,%u -- recorded OK", key, x, y, z);
        } else {
            b.cl->Dispatch(x, y, z);
        }
        HotPath::GpuBracketEnd(b.cl, hp);
        g_gpu_timer.End(b.cl, gt);
        SkCapAfterDispatch(b.device, b.cl, x, y, z);
    }
    ++g_dispatches_issued; // POSITIVE proof, see DispatchesIssued()

    if (FrameAnalysisArmed()) {
        uint64_t cs_id = 0;
        if (ShaderCode* sc = AsShaderCode(State().shader[5])) {
            static std::mutex                                      idm;
            static std::unordered_map<const ShaderCode*, uint64_t> ids;
            std::lock_guard<std::mutex>                            l(idm);
            auto                                                   it = ids.find(sc);
            if (it != ids.end())
                cs_id = it->second;
            else {
                uint64_t h = Fnv1a64(sc->code.data(), sc->code.size());
                if (!h) h = 1; // never hand back 0: their dump skips a compute shader's inputs on 0
                ids.emplace(sc, h);
                cs_id = h;
            }
        }
        DrawCallInfo dci{};
        dci.type = DrawCall::Dispatch;
        dci.ThreadGroupCountX = x;
        dci.ThreadGroupCountY = y;
        dci.ThreadGroupCountZ = z;
        FrameAnalysisAfterDraw(true, &dci, 0, 0, cs_id);
    }

    if (g_uav_barrier) {
        PipeState& pst = State();
        for (const RootBind& rb : want) {
            if (rb.kind != 'u' || rb.reg >= kMaxUAV) continue;
            WrapView* wv = AsWrapView(pst.uav[rb.reg]);
            if (wv && wv->wres && wv->wres->dx12) g_owed_uav.Note(b.cl, wv->wres->dx12);
        }
    }

    OweGameState(b);
    return true;
}

unsigned long long DispatchesIssued()
{
    return g_dispatches_issued.load();
}
unsigned long long DispatchCalls()
{
    return g_dispatch_calls.load();
}

static std::mutex              g_retire_mtx;
static std::vector<RetiredBuf> g_retired_res; // wrapper resources whose last reference dropped

void RetireDx12Resource(ID3D12Resource* res, ResourceAllocation memory)
{
    if (!res) return;
    std::lock_guard<std::mutex> l(g_retire_mtx);
    g_retired_res.push_back({ res, GpuStamp(), memory });
}

void UploadFrameTick()
{
    std::vector<RetiredBuf> gone;
    {
        std::lock_guard<std::mutex> l(g_retire_mtx);
        size_t                      keep = 0;
        for (size_t i = 0; i < g_retired_res.size(); ++i) {
            if (GpuPassed(g_retired_res[i].stamp))
                gone.push_back(g_retired_res[i]);
            else
                g_retired_res[keep++] = g_retired_res[i];
        }
        g_retired_res.resize(keep);
    }
    for (const auto& r : gone) {
        r.res->Release();
        ResourceMemory().Released(r.memory);
    }
}

} // namespace MigotoPort
